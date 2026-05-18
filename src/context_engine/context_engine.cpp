#include "src/context_engine/context_engine.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "src/context_engine/db_storage.h"
#include "src/context_engine/json_storage.h"
#include "src/context_engine/storage_base.h"
#include "src/utils/data_dir.h"
#include "src/utils/logger.h"

namespace fs = std::filesystem;

namespace jiuwen {

ContextEngine::ContextEngine(const ContextConfig& config)
    : config_(config)
{
}

ContextEngine::~ContextEngine()
{
}

bool ContextEngine::Initialize()
{
    switch (config_.storageType) {
        case ContextConfig::StorageType::DATABASE:
            storage_ = std::make_unique<DbStorage>(config_.storagePath, config_.sessionId);
            break;
        case ContextConfig::StorageType::JSON_FILE:
            storage_ = std::make_unique<JsonStorage>(config_.storagePath, config_.sessionId);
            break;
        case ContextConfig::StorageType::MEMORY_ONLY:
        default:
            storage_ = nullptr;
            break;
    }

    if (storage_) {
        return storage_->LoadHistory(memoryBuffer_);
    }
    return true;
}

void ContextEngine::AddMessage(const Message& message)
{
    // Reject empty messages at the engine level; they corrupt context history
    if (message.content.empty()) return;

    memoryBuffer_.push_back(message);
    if (storage_) {
        storage_->SaveMessage(message);
    }
}

std::vector<Message> ContextEngine::GetContextWindow() const
{
    if (memoryBuffer_.empty()) return {};
    return ApplyContextLimits(memoryBuffer_);
}

int ContextEngine::CalculateMessageTokens(const Message& msg) const
{
    return EstimateTokens(msg.content) + EstimateTokens(msg.role);
}

std::vector<Message> ContextEngine::ApplyContextLimits(const std::vector<Message>& messages) const
{
    if (messages.empty()) return {};

    std::vector<Message> result;
    int totalTokens = 0;

    // If message count exceeds maxMessages, apply trimming strategy
    if (static_cast<int>(messages.size()) > config_.maxMessages) {
        // Always keep the first message (usually user's initial query)
        // And keep the most recent messages within limits
        int messagesToKeep = config_.maxMessages;
        
        // Reserve space for first message + recent messages
        result.reserve(messagesToKeep + 1);
        
        // Add first message
        result.push_back(messages[0]);
        totalTokens += CalculateMessageTokens(messages[0]);
        
        // Add recent messages from the end, respecting token limit
        int startIndex = static_cast<int>(messages.size()) - 1;
        int keptCount = 0;
        
        for (int i = startIndex; i > 0 && keptCount < messagesToKeep - 1; --i) {
            int msgTokens = CalculateMessageTokens(messages[i]);
            if (totalTokens + msgTokens > config_.maxContextTokens) {
                break;
            }
            result.push_back(messages[i]);
            totalTokens += msgTokens;
            keptCount++;
        }
        
        // Reverse the recent messages to maintain order
        if (result.size() > 1) {
            std::vector<Message> recent(result.begin() + 1, result.end());
            std::reverse(recent.begin(), recent.end());
            result.erase(result.begin() + 1, result.end());
            result.insert(result.end(), recent.begin(), recent.end());
        }
    } else {
        // Within message limit, just apply token limit
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            int msgTokens = CalculateMessageTokens(*it);
            if (!result.empty() && (totalTokens + msgTokens > config_.maxContextTokens)) {
                break;
            }
            result.insert(result.begin(), *it);
            totalTokens += msgTokens;
        }
    }

    return result;
}

std::string ContextEngine::GetContextAsString() const
{
    auto messages = GetContextWindow();
    if (messages.empty()) return "";
    std::ostringstream oss;
    for (const auto& msg : messages) {
        oss << msg.role << ": " << msg.content << "\n";
    }
    return oss.str();
}

std::vector<Message> ContextEngine::GetAllMessages() const
{
    return memoryBuffer_;
}

void ContextEngine::Clear()
{
    memoryBuffer_.clear();
    if (storage_) {
        storage_->Clear();
    }
}

int ContextEngine::GetTokenCount() const
{
    int total = 0;
    for (const auto& msg : memoryBuffer_) {
        total += EstimateTokens(msg.content) + EstimateTokens(msg.role);
    }
    return total;
}

std::string ContextEngine::GetSessionId() const
{
    return config_.sessionId;
}

int ContextEngine::EstimateTokens(const std::string& text)
{
    return static_cast<int>(text.length()) / 4;
}

std::string ContextEngine::LoadMemoryContext() const
{
    std::lock_guard<std::mutex> lock(memoryMutex_);
    fs::path memoryPath = fs::path(GetDataDir().GetMemoryPath());

    if (fs::exists(memoryPath) && fs::is_regular_file(memoryPath)) {
        std::ifstream file(memoryPath);
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }
    }

    return "";
}

std::string ContextEngine::MergeMessageContent(const std::string& left, const std::string& right)
{
    if (left.empty()) return right;
    if (right.empty()) return left;
    return left + "\n\n" + right;
}

std::vector<Message> ContextEngine::BuildMessagesForLLM(
    const std::string& systemPrompt,
    const std::vector<Message>& history,
    const Message& currentMessage) const
{
    std::vector<Message> result;
    
    if (!systemPrompt.empty()) {
        result.push_back({"system", systemPrompt});
    }
    
    auto limitedHistory = ApplyContextLimits(history);
    result.insert(result.end(), limitedHistory.begin(), limitedHistory.end());
    
    if (!result.empty() && result.back().role == currentMessage.role) {
        result.back().content = MergeMessageContent(result.back().content, currentMessage.content);
    } else {
        result.push_back({currentMessage.role, currentMessage.content});
    }
    
    return result;
}

std::string ContextEngine::GetMemoryContent() const
{
    return LoadMemoryContext();
}

std::string ContextEngine::GetConsolidationPayload(int maxMessages) const
{
    if (memoryBuffer_.empty()) return "";
        
    int startIdx = 0;
    if (static_cast<int>(memoryBuffer_.size()) > maxMessages) {
        startIdx = static_cast<int>(memoryBuffer_.size()) - maxMessages;
    }
        
    std::string result;
    for (int i = startIdx; i < static_cast<int>(memoryBuffer_.size()); ++i) {
        result += memoryBuffer_[i].role + ": " + memoryBuffer_[i].content + "\n\n";
    }
    return result;
}

} // namespace jiuwen

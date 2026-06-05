#include "src/context_engine/context_engine.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
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

ContextEngine::~ContextEngine() = default;

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
        if (!storage_->LoadHistory(memoryBuffer_)) {
            return false;
        }
        // Defensive: drop trailing assistant(tool_calls) with no matching
        // tool response. This guards against histories left in a broken
        // state by a previous interrupted run.
        size_t before = memoryBuffer_.size();
        TrimOrphanTrailingToolCalls(memoryBuffer_);
        if (memoryBuffer_.size() < before) {
            LOG(WARN) << "[ContextEngine] Dropped " << (before - memoryBuffer_.size())
                      << " trailing message(s) with orphan tool_calls in session=" << config_.sessionId;
        }
    }
    return true;
}

void ContextEngine::AddMessage(const Message& message)
{
    if (!ContextStorageBase::IsValidMessage(message)) return;
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
    int total = EstimateTokens(msg.role) + EstimateTokens(msg.content);
    for (const auto& tc : msg.toolCalls) {
        total += EstimateTokens(tc.name) + EstimateTokens(tc.argumentsJson);
    }
    total += EstimateTokens(msg.toolCallId) + EstimateTokens(msg.toolName);
    return total;
}

bool ContextEngine::CanMerge(const Message& prev, const Message& cur)
{
    if (prev.role != cur.role) return false;
    // tool messages must each retain their own tool_call_id
    if (prev.role == "tool") return false;
    // assistant carrying tool_calls is structural
    if (prev.role == "assistant" && (!prev.toolCalls.empty() || !cur.toolCalls.empty())) {
        return false;
    }
    // user / system / plain assistant text: safe to merge
    return true;
}

std::vector<Message> ContextEngine::ApplyContextLimits(const std::vector<Message>& messages) const
{
    if (messages.empty()) return {};

    // 1. Safety sanitisation: merge consecutive same-role messages where
    // structural fields permit. This guards against duplicated user turns
    // (e.g. a previous empty-response interrupt) without breaking the
    // assistant(tool_calls) -> tool(result) pairing required by the API.
    std::vector<Message> sanitized;
    sanitized.reserve(messages.size());
    for (const auto& msg : messages) {
        if (!sanitized.empty() && CanMerge(sanitized.back(), msg)) {
            sanitized.back().content = MergeMessageContent(sanitized.back().content, msg.content);
        } else {
            sanitized.push_back(msg);
        }
    }

    const std::vector<Message>& workingMessages = sanitized;

    std::vector<Message> result;
    int totalTokens = 0;

    // 2. Trim by message count first (keep first + most recent).
    if (static_cast<int>(workingMessages.size()) > config_.maxMessages) {
        int messagesToKeep = config_.maxMessages;
        result.reserve(messagesToKeep + 1);
        result.push_back(workingMessages[0]);
        totalTokens += CalculateMessageTokens(workingMessages[0]);

        int startIndex = static_cast<int>(workingMessages.size()) - 1;
        int keptCount = 0;
        for (int i = startIndex; i > 0 && keptCount < messagesToKeep - 1; --i) {
            int msgTokens = CalculateMessageTokens(workingMessages[i]);
            if (totalTokens + msgTokens > config_.maxContextTokens) break;
            result.push_back(workingMessages[i]);
            totalTokens += msgTokens;
            ++keptCount;
        }
        if (result.size() > 1) {
            std::vector<Message> recent(result.begin() + 1, result.end());
            std::reverse(recent.begin(), recent.end());
            result.erase(result.begin() + 1, result.end());
            result.insert(result.end(), recent.begin(), recent.end());
        }
    } else {
        for (auto it = workingMessages.rbegin(); it != workingMessages.rend(); ++it) {
            int msgTokens = CalculateMessageTokens(*it);
            if (!result.empty() && (totalTokens + msgTokens > config_.maxContextTokens)) break;
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
        oss << msg.role;
        if (!msg.toolCalls.empty()) {
            oss << " (tool_calls=" << msg.toolCalls.size() << ")";
        }
        if (!msg.toolCallId.empty()) {
            oss << " (tool_call_id=" << msg.toolCallId << ")";
        }
        oss << ": " << msg.content << "\n";
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
        total += CalculateMessageTokens(msg);
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

void ContextEngine::TrimOrphanTrailingToolCalls(std::vector<Message>& msgs)
{
    while (!msgs.empty()) {
        const auto& last = msgs.back();
        if (last.role == "assistant" && !last.toolCalls.empty()) {
            msgs.pop_back();
        } else {
            break;
        }
    }
}

std::vector<Message> ContextEngine::BuildMessagesForLLM(
    const std::string& systemPrompt,
    const std::vector<Message>& history,
    const Message& currentMessage) const
{
    std::vector<Message> result;
    if (!systemPrompt.empty()) {
        Message sys;
        sys.role = "system";
        sys.content = systemPrompt;
        result.push_back(sys);
    }
    auto limitedHistory = ApplyContextLimits(history);
    result.insert(result.end(), limitedHistory.begin(), limitedHistory.end());
    if (!result.empty() && CanMerge(result.back(), currentMessage)) {
        result.back().content = MergeMessageContent(result.back().content, currentMessage.content);
    } else {
        result.push_back(currentMessage);
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
        const auto& m = memoryBuffer_[i];
        result += m.role + ": " + m.content + "\n\n";
    }
    return result;
}

} // namespace jiuwen


#include "src/context_engine/context_engine.h"
#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "src/context_engine/db_storage.h"
#include "src/context_engine/md_storage.h"
#include "src/context_engine/storage_base.h"

ContextEngine::ContextEngine(const ContextConfig& config) : config_(config){} ContextEngine::~ContextEngine() = default;

bool ContextEngine::Initialize()
{
    switch (config_.storageType) {
        case ContextConfig::StorageType::MARKDOWN_FILE:
            storage_ = std::make_unique<MarkdownStorage>(config_.storagePath, config_.sessionId);
            break;
        case ContextConfig::StorageType::DATABASE: {
            std::string dbPath = config_.storagePath.empty() ? "agent_context.db" : config_.storagePath;
            storage_ = std::make_unique<DbStorage>(dbPath, config_.sessionId);
            break;
        }
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
    memoryBuffer_.push_back(message);
    if (storage_) {
        storage_->SaveMessage(message);
    }
}

std::vector<Message> ContextEngine::GetContextWindow() const
{
    if (memoryBuffer_.empty()) return {};
    std::vector<Message> window;
    int currentTokens = 0;
    for (auto it = memoryBuffer_.rbegin(); it != memoryBuffer_.rend(); ++it) {
        int msgTokens = EstimateTokens(it->content) + EstimateTokens(it->role);
        if (!window.empty() && (currentTokens + msgTokens > config_.maxContextTokens)) {
            break;
        }
        window.insert(window.begin(), *it);
        currentTokens += msgTokens;
    }
    return window;
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

#include "context_engine/context_engine.h"
#include "storage_interface.h"
#include "md_storage.h"
#include "db_storage.h"
#include <algorithm>
#include <iostream>

// ... (later)

bool ContextEngine::Initialize() {
    // 1. Create Storage Backend
    switch (m_config.storageType) {
        case StorageType::MARKDOWN_FILE:
            m_storage = std::make_unique<MarkdownStorage>(m_config.storagePath, m_config.sessionId);
            break;
        case StorageType::DATABASE:
        {
            std::string dbPath = m_config.storagePath.empty() ? "agent_context.db" : m_config.storagePath;
            m_storage = std::make_unique<DbStorage>(dbPath, m_config.sessionId);
            break;
        }
        case StorageType::MEMORY_ONLY:
        default:
            m_storage = nullptr;
            break;
    }

    // 2. Load history from storage into memory if available
    if (m_storage) {
        return m_storage->LoadHistory(m_memoryBuffer);
    }
    return true;
}

void ContextEngine::AddMessage(const Message& message) {
    m_memoryBuffer.push_back(message);
    
    // Persist to storage if configured
    if (m_storage) {
        m_storage->SaveMessage(message);
    }
}

std::vector<Message> ContextEngine::GetContextWindow() const {
    if (m_memoryBuffer.empty()) return {};
    
    std::vector<Message> window;
    int currentTokens = 0;
    
    // Walk backwards from most recent
    for (auto it = m_memoryBuffer.rbegin(); it != m_memoryBuffer.rend(); ++it) {
        int msgTokens = EstimateTokens(it->content) + EstimateTokens(it->role);
        if (!window.empty() && (currentTokens + msgTokens > m_config.maxContextTokens)) {
            break;
        }
        window.insert(window.begin(), *it);
        currentTokens += msgTokens;
    }
    return window;
}

std::vector<Message> ContextEngine::GetAllMessages() const {
    return m_memoryBuffer;
}

void ContextEngine::Clear() {
    m_memoryBuffer.clear();
    if (m_storage) {
        m_storage->Clear();
    }
}

int ContextEngine::GetTokenCount() const {
    int total = 0;
    for (const auto& msg : m_memoryBuffer) {
        total += EstimateTokens(msg.content) + EstimateTokens(msg.role);
    }
    return total;
}

std::string ContextEngine::GetSessionId() const {
    return m_config.sessionId;
}

int ContextEngine::EstimateTokens(const std::string& text) {
    return static_cast<int>(text.length()) / 4;
}

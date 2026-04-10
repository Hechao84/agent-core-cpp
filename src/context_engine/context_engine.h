#pragma once
#include <string>
#include <vector>
#include <memory>
#include "model.h" // For Message struct
#include "types.h" // For ContextConfig and StorageType enums

class ContextStorageInterface; // Forward declaration

class ContextEngine {
public:
    explicit ContextEngine(const ContextConfig& config);
    ~ContextEngine();

    // Initialize engine (loads history from storage if configured)
    bool Initialize();

    // Core operations
    void AddMessage(const Message& message);
    std::vector<Message> GetContextWindow() const;
    std::string GetContextAsString() const; // New helper for prompt injection
    std::vector<Message> GetAllMessages() const;
    void Clear();

    // Info
    int GetTokenCount() const;
    std::string GetSessionId() const;

private:
    ContextConfig m_config;
    std::vector<Message> m_memoryBuffer;
    std::unique_ptr<ContextStorageInterface> m_storage;

    static int EstimateTokens(const std::string& text);
};

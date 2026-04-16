#pragma once

#include <memory>
#include <string>
#include <vector>
#include "include/model.h"
#include "include/types.h"

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
    ContextConfig config_;
    std::vector<Message> memoryBuffer_;
    std::unique_ptr<ContextStorageInterface> storage_;

    static int EstimateTokens(const std::string& text);
};

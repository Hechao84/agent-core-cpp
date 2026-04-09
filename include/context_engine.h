#pragma once
#include <string>
#include <vector>
#include <memory>
#include "model.h"

enum class ContextStorageType {
    FILE_MD,
    DATABASE,
};

struct ContextConfig {
    ContextStorageType storageType{ContextStorageType::FILE_MD};
    std::string storagePath; // Directory for file storage or connection string for DB
    int maxContextTokens{4096}; // Max tokens for context window
    std::string sessionId;
};

class ContextEngine {
public:
    virtual ~ContextEngine() = default;
    
    // Initialize the context engine with config
    virtual bool Initialize(const ContextConfig& config) = 0;
    
    // Append a message to the context
    virtual bool AppendMessage(const Message& message) = 0;
    
    // Get the current context window (recent messages within token limit)
    virtual std::vector<Message> GetContextWindow() const = 0;
    
    // Get all messages in the context
    virtual std::vector<Message> GetAllMessages() const = 0;
    
    // Clear the context
    virtual void Clear() = 0;
    
    // Get current token count estimate
    virtual int GetCurrentTokenCount() const = 0;
    
    // Get session ID
    virtual std::string GetSessionId() const = 0;
    
    // Factory method to create context engine based on config
    static std::unique_ptr<ContextEngine> Create(const ContextConfig& config);
};

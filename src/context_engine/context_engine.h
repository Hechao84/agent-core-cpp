#pragma once
#include <string>
#include <vector>
#include <memory>
#include "model.h" // For Message struct
#include "types.h" // For ModelConfig if needed, though Message is likely in model.h or types.h

// Assuming Message is defined in model.h or similar. 
// If Message is not available here, we might need to include the right header.
// Looking at previous code, Message seems to be used in model.h.

enum class StorageType {
    MEMORY_ONLY,
    MARKDOWN_FILE,
    DATABASE
};

struct ContextConfig {
    StorageType storageType{StorageType::MARKDOWN_FILE};
    std::string storagePath; 
    int maxContextTokens{4096}; 
    std::string sessionId;
};

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

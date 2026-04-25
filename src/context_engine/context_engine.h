#pragma once

#include <memory>
#include <string>
#include <vector>
#include "include/model.h"
#include "include/types.h"

namespace jiuwen {

class ContextStorageBase; // Forward declaration

class ContextEngine {
public:
    explicit ContextEngine(const ContextConfig& config);
    ~ContextEngine();

    bool Initialize();

    void AddMessage(const Message& message);
    std::vector<Message> GetContextWindow() const;
    std::string GetContextAsString() const;
    std::vector<Message> GetAllMessages() const;
    void Clear();

    int GetTokenCount() const;
    std::string GetSessionId() const;

    std::vector<Message> BuildMessagesForLLM(const std::string& systemPrompt, const std::vector<Message>& history, const Message& currentMessage) const;

    void UpdateMemory(const std::string& keyFacts);
    void OverwriteMemory(const std::string& fullContent);
    void ClearMemory();
    std::string GetMemoryContent() const;
    std::string GetConsolidationPayload(int maxMessages = 100) const;

private:
    ContextConfig config_;
    std::vector<Message> memoryBuffer_;
    std::unique_ptr<ContextStorageBase> storage_;

    static int EstimateTokens(const std::string& text);
    int CalculateMessageTokens(const Message& msg) const;
    std::vector<Message> ApplyContextLimits(const std::vector<Message>& messages) const;
    
    std::string LoadMemoryContext() const;
    static std::string MergeMessageContent(const std::string& left, const std::string& right);
};

} // namespace jiuwen

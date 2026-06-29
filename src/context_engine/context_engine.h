#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "include/memory_types.h"
#include "include/model.h"
#include "include/types.h"

namespace jiuwen {

class ContextStorageBase;

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

    std::vector<Message> BuildMessagesForLLM(const std::string& systemPrompt,
                                               const std::vector<Message>& history,
                                               const Message& currentMessage) const;

    std::string GetMemoryContent() const;
    void SetMemoryContextProvider(std::function<std::string()> provider);
    void SetMemoryEventSink(std::function<void(const MemoryEvent&)> sink);
    std::string GetConsolidationPayload(int maxMessages = 100) const;

private:
    ContextConfig config_;
    std::vector<Message> memoryBuffer_;
    std::unique_ptr<ContextStorageBase> storage_;
    std::function<std::string()> memoryContextProvider_;
    std::function<void(const MemoryEvent&)> memoryEventSink_;
    mutable std::mutex memoryMutex_;  // Lock layer L6 (message buffer + storage)

    struct MessageSegment;

    static int EstimateTokens(const std::string& text);
    int CalculateMessageTokens(const Message& msg) const;
    int CalculateMessagesTokens(const std::vector<Message>& messages) const;
    int CalculateMessagesTokens(std::vector<Message>::const_iterator begin,
                                std::vector<Message>::const_iterator end) const;
    std::vector<Message> ApplyContextLimits(const std::vector<Message>& messages) const;
    std::vector<MessageSegment> BuildMessageSegments(const std::vector<Message>& messages) const;
    std::vector<Message> CompressSegment(const std::vector<Message>& messages, const MessageSegment& segment, int tokenBudget) const;

    std::string LoadMemoryContext() const;

    // Whether two adjacent messages can be safely merged. user-user and
    // text-only assistant-assistant pairs may be merged (robustness against
    // duplicate / interrupted turns); messages carrying tool_calls or
    // tool_call_id are never merged because they have structural meaning.
    static bool CanMerge(const Message& prev, const Message& cur);
    static std::string MergeMessageContent(const std::string& left, const std::string& right);

    static int DropUnpairedToolMessages(std::vector<Message>& msgs);
    static int DropOrphanToolCalls(std::vector<Message>& msgs);
    static void TrimOrphanTrailingToolCalls(std::vector<Message>& msgs);
};

} // namespace jiuwen

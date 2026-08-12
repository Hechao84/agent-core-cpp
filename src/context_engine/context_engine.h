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

    // The current user query drives relevance-scoped retrieval inside the
    // memory runtime (long-term entity/relation search and payload
    // filtering). An empty query reproduces the legacy "dump recent N"
    // behavior, so callers without a query can pass an empty string.
    std::string GetMemoryContent(const std::string& query) const;
    void SetMemoryContextProvider(std::function<std::string(const std::string&)> provider);
    void SetMemoryEventSink(std::function<void(const MemoryEvent&)> sink);
    std::string GetConsolidationPayload(int maxMessages = 100) const;

    // V3 (round5 §5.4.2): exposed public so AgentWorker::ResolveByBudget can
    // estimate tool schema / catalog token counts for AUTO mode budget
    // resolution. Pure utility (no instance state, len/4 coarse estimate
    // with CJK-friendly ascii/4 + wide*3/4 weighting). Made public rather
    // than duplicating the estimation logic in AgentWorker.
    static int EstimateTokens(const std::string& text);

private:
    ContextConfig config_;
    std::vector<Message> memoryBuffer_;
    std::unique_ptr<ContextStorageBase> storage_;
    std::function<std::string(const std::string&)> memoryContextProvider_;
    std::function<void(const MemoryEvent&)> memoryEventSink_;
    mutable std::mutex memoryMutex_;  // Lock layer L6 (message buffer + storage)

    struct MessageSegment;

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

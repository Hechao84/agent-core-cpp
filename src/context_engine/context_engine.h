#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
    std::string GetConsolidationPayload(int maxMessages = 100) const;

private:
    ContextConfig config_;
    std::vector<Message> memoryBuffer_;
    std::unique_ptr<ContextStorageBase> storage_;
    mutable std::mutex memoryMutex_;

    static int EstimateTokens(const std::string& text);
    int CalculateMessageTokens(const Message& msg) const;
    std::vector<Message> ApplyContextLimits(const std::vector<Message>& messages) const;

    std::string LoadMemoryContext() const;

    // Whether two adjacent messages can be safely merged. user-user and
    // text-only assistant-assistant pairs may be merged (robustness against
    // duplicate / interrupted turns); messages carrying tool_calls or
    // tool_call_id are never merged because they have structural meaning.
    static bool CanMerge(const Message& prev, const Message& cur);
    static std::string MergeMessageContent(const std::string& left, const std::string& right);

    // Strip a trailing assistant(tool_calls) that has no matching tool
    // response. Such "orphan tool_calls" would be rejected by OpenAI / cause
    // models to refuse to generate, and arise when a previous run was killed
    // mid-tool-execution.
    static void TrimOrphanTrailingToolCalls(std::vector<Message>& msgs);
};

} // namespace jiuwen

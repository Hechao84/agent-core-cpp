#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include "include/model.h"
#include "include/types.h"
#include "src/tools/tool_selector.h"

namespace jiuwen {

class ContextEngine;
class SkillEngine;
class WorkerEnv;

class AgentWorker {
public:
    explicit AgentWorker(AgentConfig config);
    virtual ~AgentWorker() = default;

    virtual std::string Invoke(const std::string& query, ContextEngine* contextEngine,
                                std::function<void(const std::string&)> callback) = 0;

    virtual void Cancel();
    void AddTools(const std::vector<std::string>& toolNames);
    void RemoveTools(const std::vector<std::string>& toolNames);
    void SetSkillEngine(std::shared_ptr<SkillEngine> engine);
    void SetWorkerEnv(WorkerEnv* env);

protected:
    AgentConfig config_;
    mutable std::mutex toolMutex_;
    std::atomic<uint64_t> cancelGeneration_{0};
    std::vector<std::string> toolNames_;
    std::unique_ptr<ToolSelector> toolSelector_;
    std::shared_ptr<SkillEngine> skillEngine_;
    WorkerEnv* workerEnv_{nullptr};

    // Call the configured model with structured messages and tool schemas
    // (when useNativeFunctionCalling is true). Returns the structured response
    // including any tool_calls produced by the model. onChunk receives only
    // text deltas; tool_calls are buffered and returned in the response.
    ModelResponse CallModelStream(const std::string& systemPrompt,
                                   const std::vector<Message>& messages,
                                   std::function<void(const std::string&)> onChunk,
                                   uint64_t generation = 0);

    // Build native tool schemas for all tools currently registered with this
    // worker. The session context is needed to construct session-scoped
    // tools (their schema is independent of the per-call ctx values).
    std::vector<ToolSchema> BuildToolSchemas() const;

    std::string BuildPrompt(const std::string& templateName, const std::string& query,
                            const std::string& context, ContextEngine* contextEngine);

    std::string ExecuteTool(const std::string& toolName, const std::string& input,
                            const std::function<void(const std::string&)>& streamCallback);
    std::string GetToolSchemaForQuery(const std::string& query);
    bool IsCancelled(uint64_t myGeneration) const;

    std::string GetTodoSnippet() const;

    std::uint64_t StartNewInvocation();
};

std::unique_ptr<AgentWorker> CreateAgentWorker(AgentConfig config);

} // namespace jiuwen

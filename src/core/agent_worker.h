#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "include/model.h"
#include "include/types.h"
#include "src/core/capability_selector.h"

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
    // Snapshot of the currently-enabled tool names (under toolMutex_).
    std::vector<std::string> GetToolNames() const;
    // Reconcile MCP tools with the live ResourceManager registry: add newly-
    // registered MCP tools, drop ones whose server is gone. All under
    // toolMutex_ (atomic vs Invoke). Returns the add+remove delta.
    int SyncMcpTools();
    void SetSkillEngine(std::shared_ptr<SkillEngine> engine);
    void SetWorkerEnv(WorkerEnv* env);

protected:
    AgentConfig config_;
    mutable std::mutex toolMutex_;  // Lock layer L5 (tool names + selector + mcp ownership)
    std::atomic<uint64_t> cancelGeneration_{0};
    std::vector<std::string> toolNames_;
    std::vector<std::string> ownedMcpTools_;  // MCP tools this worker added (for SyncMcpTools diff)
    // V2 (round5 §5.4.1 条 10): LLM-backed capability recall engine, replaces
    // the deprecated ToolSelector (whose selection methods were stubs and
    // whose pool only held names). Held here so Invoke entry (react_worker.cpp)
    // can call findRelevant once at turn-start under SELECTIVE mode.
    // Forward-declared above; full type in src/core/capability_selector.h.
    std::unique_ptr<CapabilitySelector> capabilitySelector_;
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
    // Returns true when progressive disclosure is active (PROGRESSIVE or
    // SELECTIVE mode). AUTO resolves to DISABLED in v1 (no budget-driven
    // selection yet), so it returns false for AUTO. Centralizes the mode
    // branch so BuildToolSchemas/BuildPrompt/Invoke share one resolution point.
    bool IsProgressiveDisclosureActive() const;
    // alwaysOn = MetaToolNames() ∪ config_.alwaysOnTools, via
    // ComputeAlwaysOnFor(config_). Computed here (AgentWorker holds config_)
    // and consumed by BuildToolSchemas (FC = alwaysOn ∪ loadedTools) and
    // BuildPrompt (visible = active ∪ alwaysOn, callable = alwaysOn ∪
    // loaded).
    std::set<std::string> ComputeAlwaysOn() const;
    // Returns true when the invocation identified by myGeneration has been
    // cancelled (i.e. Cancel() advanced cancelGeneration_ past myGeneration).
    bool IsCancelled(uint64_t myGeneration) const;

    std::string GetTodoSnippet() const;

    // Snapshot the current cancel generation to use as this invocation's
    // baseline. Does NOT increment - the counter only moves on Cancel() - so
    // concurrent invocations sharing this worker are not invalidated.
    std::uint64_t CurrentCancelGeneration();
};

std::unique_ptr<AgentWorker> CreateAgentWorker(AgentConfig config);

// The meta-tools that the progressive-disclosure mechanism itself introduces
// (the escape valves: tool_search for tool recall, skill_search for skill
// recall). They are always in the alwaysOn set under PROGRESSIVE/SELECTIVE
// modes (otherwise the escape valve deadlocks). Single source for this list
// — consumed by ComputeAlwaysOnFor() below and by SessionManager's
// tool_search registration lambda (which injects the full alwaysOn set into
// ToolSearchTool so its load action can idempotently short-circuit on ANY
// alwaysOn name, not just the meta-tools). Returns a reference to a static
// set so callers can compare addresses / use without copying.
const std::set<std::string>& MetaToolNames();

// alwaysOn = MetaToolNames() ∪ config.alwaysOnTools. Free function form so
// callers without an AgentWorker instance (notably SessionManager's
// tool_search registration lambda, which has only the AgentConfig at
// registration time) can compute the same set AgentWorker::ComputeAlwaysOn()
// produces. Keeps the meta-tools name list and the union logic in one place.
std::set<std::string> ComputeAlwaysOnFor(const AgentConfig& config);

} // namespace jiuwen

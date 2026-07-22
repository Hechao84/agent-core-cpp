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
    // V3 (round5 §5.4.2): read effective mode — resolves lazily if AUTO.
    // All sites that branched on config_.toolDisclosureMode now read this
    // (Invoke seed, BuildToolSchemas, BuildPrompt, registration sites).
    // Triggers IsProgressiveDisclosureActive() for its lazy-resolution side
    // effect (the bool return value of that call is ignored — only the
    // side effect of populating effectiveMode_ matters).
    ToolDisclosureMode GetEffectiveMode() const;

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
    // V3 (round5 §5.4.2): AUTO mode lazy resolution. effectiveMode_ starts
    // as config_.toolDisclosureMode (AUTO stays unresolved until first
    // IsProgressiveDisclosureActive() call). resolveOnce_ gates one-shot
    // resolution under std::call_once (multi-session concurrent first-call
    // safe; mutable because resolution is logical-const — does not change
    // observable state — same precedent as cancelGeneration_ atomic).
    mutable ToolDisclosureMode effectiveMode_{config_.toolDisclosureMode};
    mutable std::once_flag resolveOnce_;

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
    // SELECTIVE mode). V3: AUTO triggers lazy resolution via call_once on
    // first call (effectiveMode_ resolved to one of DISABLED/PROGRESSIVE/
    // SELECTIVE based on pool token budget), then this returns whether the
    // resolved mode is PROGRESSIVE/SELECTIVE. PROG/SEL skip resolution
    // (effectiveMode_ already equals config_.toolDisclosureMode). Centralizes
    // the mode branch so BuildToolSchemas/BuildPrompt/Invoke share one
    // resolution point.
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

private:
    // V3 (round5 §5.4.2 条 12): AUTO budget→mode resolution, data-fetch
    // phase. Calls ResourceManager (RM singleton, non-pure), so stays as
    // member. Pool snapshot via GetToolNames() (no separate snapshotPool()
    // helper). Tier 2 excludes alwaysOn (ComputeAlwaysOn()) since meta-
    // tools + user-configured alwaysOnTools are always FC-resident
    // regardless of mode — counting them would inflate Tier 2 and may
    // falsely elevate a disabled-sized pool to progressive.
    ToolDisclosureMode ResolveByBudget(const std::vector<std::string>& pool) const;
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

// V3 (round5 §5.4.2 条 12): AUTO budget→mode resolution, judge phase.
// Pure function (eats 4 ints, returns ToolDisclosureMode, no RM side effects).
// Free function form (header-declared, .cpp-defined, non-static, non-anon
// namespace) so unit tests can call directly — same pattern as MetaToolNames
// above. Not part of the SDK public API; exposed in header only for testability.
//
// tier2/tier1 are estimated token counts; schemaBudget/catalogBudget are
// config thresholds (0 = "this tier's budget unset, skip judgment" — that
// tier never triggers elevation). Both budgets 0 → DISABLED (default AUTO
// config with no explicit budgets = zero +1 LLM/turn, conservative).
ToolDisclosureMode ResolveModeByTokenBudget(int tier2, int tier1,
                                            int schemaBudget, int catalogBudget);

} // namespace jiuwen

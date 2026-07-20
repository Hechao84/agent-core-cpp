#pragma once

#include <set>
#include <string>
#include <vector>

namespace jiuwen {

// Per-turn tool disclosure state, accessed by the tool_search tool and by
// AgentWorker::BuildToolSchemas via WorkerEnv::GetCurrentTurnState(). Both
// consumers must reach the SAME per-turn state object (the proxy owned by
// the current SessionEntry): tool_search writes loadedTools/activeSet
// through it, BuildToolSchemas reads loadedTools to rebuild the FC array
// each React iteration. The concrete implementation is a proxy stored on
// SessionEntry (see src/core/tool_turn_state_proxy.h). The interface has 7
// methods split by caller: the first 4 (load/search/getActiveSet/
// getLoadedTools) are tool_search-facing; the last 3 (reset/seedActive/
// isActiveFullPool) are AgentWorker/runtime-facing (Invoke entry reset+seed
// and the search short-circuit signal). The interface exists to decouple
// tool_search from SessionEntry's full surface and give BuildToolSchemas/
// tool_search a stable, narrow handle to the per-turn state.
//
// Lifecycle: per-session (the proxy is lazily created once per
// SessionEntry), per-turn reset (activeSet/loadedTools are cleared at
// Invoke entry; the proxy object itself is NOT rebuilt across turns).
// NOTE: this interface is intentionally an internal decoupling seam, not part
// of the SDK public API. It is NOT exported (no AGENT_API) and lives under
// src/core/, not include/. Rationale: all consumers needing the complete
// type (tool_search_tool.h, agent_worker.cpp, react_worker.cpp, the proxy and
// tests) are inside src/ or unittest/. The public headers resource_manager.h
// and session_manager.h hold only a forward declaration (class ToolTurnState;)
// and use it via pointer / unique_ptr, so they compile without this header.
// Pure-virtual methods have no out-of-line symbol to export — virtual dispatch
// resolves through the proxy's vtable, which is already internal. Keeping
// this out of include/ avoids freezing an implementation detail into a public
// ABI contract; v2 (e.g. seedActiveSubset) can then be added freely.
class ToolTurnState {
public:
    virtual ~ToolTurnState() = default;

    // Load a tool's name into loadedTools (FC array). Validates the name is
    // a registered tool upstream (tool_search_tool.cpp calls
    // ResourceManager::HasTool/HasSessionTool before invoking this). In
    // selective mode the name is also pulled into activeSet to preserve
    // monotonicity; in progressive it is already present (active = full
    // pool). Idempotent for names already in loadedTools (set semantics).
    virtual void load(const std::string& name) = 0;

    // Real recall over the full pool (v2; v1's progressive short-circuits
    // in the tool layer before calling this). Returns the names added to
    // activeSet (deduped against existing entries). The tool layer renders
    // the returned names into the response text.
    virtual std::vector<std::string> search(const std::string& query) = 0;

    // Read-only views used by AgentWorker::BuildPrompt (Tier 1 catalog
    // rendering) and BuildToolSchemas (FC = alwaysOn ∪ loadedTools).
    virtual const std::set<std::string>& getActiveSet() const = 0;
    virtual const std::set<std::string>& getLoadedTools() const = 0;

    // Per-turn reset: clear activeSet and loadedTools contents. Called by
    // ReactAgentWorker::Invoke at entry (before ReactLoop) so the previous
    // turn's state does not leak into this turn. The proxy OBJECT is not
    // rebuilt (it stays valid for the session); only the set contents are
    // cleared. Not used by the tool_search tool — it is an AgentWorker-level
    // operation; included here because the proxy is the only handle Invoke
    // has to the SessionEntry's per-turn state.
    virtual void reset() = 0;

    // Seed activeSet with the full pool (all enabled tool names) WITHOUT
    // touching loadedTools. Called by Invoke at entry in progressive mode
    // (and selective v1, which falls back to progressive seeding since
    // findRelevant is v2). Distinct from load(): load() writes to BOTH
    // activeSet and loadedTools (promoting to FC), which would defeat
    // progressive disclosure if used for the full-pool seed. AgentWorker-
    // level operation, not used by tool_search.
    virtual void seedActive(const std::vector<std::string>& names) = 0;

    // Reports whether the active set was seeded as the full pool (progressive
    // always; selective v1 fallback too; selective v2's findRelevant subset
    // does not). tool_search.search uses this to short-circuit (full pool =>
    // nothing left to discover beyond active) vs invoke real recall
    // (subset => search may find more). State-based rather than mode-label-
    // based: the v1-selective-fallback and v2-selective share the SELECTIVE
    // label but differ in active-set state, so the label alone cannot
    // distinguish them — this runtime signal does. v2's findRelevant seed
    // path sets this false; v1 never does (only seedActive runs), so v1
    // short-circuits for both progressive and selective. AgentWorker-facing
    // state query, not used by tool_search's load action.
    virtual bool isActiveFullPool() const = 0;
};

} // namespace jiuwen

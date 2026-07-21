#pragma once

#include <set>
#include <string>
#include <vector>

namespace jiuwen {

// Per-turn capability disclosure state (tool + skill active sets), accessed
// by tool_search/skill_search and by AgentWorker::BuildToolSchemas/
// BuildPrompt via WorkerEnv::GetCurrentTurnState(). All consumers must reach
// the SAME per-turn state object (the proxy owned by the current
// SessionEntry): tool_search writes loadedTools/activeSet through it,
// BuildToolSchemas reads loadedTools to rebuild the FC array each React
// iteration. The concrete implementation is a proxy stored on SessionEntry
// (see src/core/turn_state_proxy.h). The interface is split by caller:
// - tool_search-facing: load/search/getActiveSet/getLoadedTools
// - skill_search-facing: searchSkill/getSkillActiveSet
// - AgentWorker/runtime-facing: reset/seedActive/seedActiveSubset/
//   isActiveFullPool/seedSkillActive/seedSkillActiveSubset/isActiveFullSkillPool
//
// Lifecycle: per-session (the proxy is lazily created once per
// SessionEntry), per-turn reset (activeSet/loadedTools/skillActiveSet are
// cleared at Invoke entry; the proxy object itself is NOT rebuilt across
// turns).
//
// V2 design notes (round5 §5.4.1):
// - seedActiveSubset mirrors seedActive but sets activeIsFullPool_=false
//   (findRelevant subset seed path). v1 only had seedActive (always true).
// - skill side is symmetric with tool side EXCEPT for load: tool has
//   load/getLoadedTools (FC hard-constraint requires delayed FC injection),
//   skill has none (skill Tier2 is stateless one-shot GetSkillInstructions,
//   no delayed state). See §5.4.1 条 11 "tool/skill load 不对称".
//
// NOTE: this interface is intentionally an internal decoupling seam, not part
// of the SDK public API. It is NOT exported (no AGENT_API) and lives under
// src/core/, not include/. Rationale: all consumers needing the complete
// type (tool_search_tool.h, skill_search_tool.h, agent_worker.cpp,
// react_worker.cpp, the proxy and tests) are inside src/ or unittest/. The
// public headers resource_manager.h and session_manager.h hold only a forward
// declaration (class TurnState;) and use it via pointer / unique_ptr, so
// they compile without this header. Pure-virtual methods have no out-of-line
// symbol to export — virtual dispatch resolves through the proxy's vtable,
// which is already internal. Keeping this out of include/ avoids freezing an
// implementation detail into a public ABI contract.
class TurnState {
public:
    virtual ~TurnState() = default;

    // --- tool_search-facing ---

    // Load a tool's name into loadedTools (FC array). Validates the name is
    // a registered tool upstream (tool_search_tool.cpp calls
    // ResourceManager::HasTool/HasSessionTool before invoking this). In
    // selective mode the name is also pulled into activeSet to preserve
    // monotonicity; in progressive it is already present (active = full
    // pool). Idempotent for names already in loadedTools (set semantics).
    virtual void load(const std::string& name) = 0;

    // Real recall over the full tool pool. v1 returns empty (stub); v2's
    // progressive short-circuits in the tool layer before calling this.
    // Returns the names added to activeSet (deduped against existing
    // entries). The tool layer renders the returned names into the response
    // text. v2's actual LLM-backed recall is invoked by tool_search_tool
    // via CapabilitySelector (not by this method directly).
    virtual std::vector<std::string> search(const std::string& query) = 0;

    // Read-only views used by AgentWorker::BuildPrompt (Tier 1 catalog
    // rendering) and BuildToolSchemas (FC = alwaysOn ∪ loadedTools).
    virtual const std::set<std::string>& getActiveSet() const = 0;
    virtual const std::set<std::string>& getLoadedTools() const = 0;

    // --- skill_search-facing (V2, symmetric with tool side except load) ---

    // Real recall over the full skill pool. v1 returns empty (stub); v2's
    // progressive short-circuits in the tool layer before calling this.
    // Returns the names added to skillActiveSet (deduped against existing
    // entries). v2's actual LLM-backed recall is invoked by
    // skill_search_tool via CapabilitySelector (not by this method directly).
    virtual std::vector<std::string> searchSkill(const std::string& query) = 0;

    virtual const std::set<std::string>& getSkillActiveSet() const = 0;

    // --- AgentWorker/runtime-facing (tool_search/skill_search do not call) ---

    // Per-turn reset: clear activeSet, loadedTools, skillActiveSet contents.
    // Called by ReactAgentWorker::Invoke at entry (before ReactLoop) so the
    // previous turn's state does not leak into this turn. The proxy OBJECT
    // is not rebuilt (it stays valid for the session); only the set contents
    // are cleared. Also resets both full-pool flags to false.
    virtual void reset() = 0;

    // Seed activeSet with the full tool pool WITHOUT touching loadedTools.
    // Called by Invoke at entry in progressive mode (and selective v2's
    // 降级 / 全相关 paths, see §5.4.1 条 6 降级判定表). Sets
    // activeIsFullPool_=true (drives tool_search.search short-circuit).
    // AgentWorker-level operation, not used by tool_search.
    virtual void seedActive(const std::vector<std::string>& names) = 0;

    // Seed activeSet with a findRelevant subset. Sets activeIsFullPool_=
    // false (so tool_search.search routes to real recall). Does NOT touch
    // loadedTools (findRelevant predicts relevance, not necessity; pre-
    // loading full schemas would waste context — see §7 决策1). Idempotent
    // for names already present (set semantics).
    // v2-only (§5.4.1 条 1): v1's seedActive unconditionally sets the flag
    // true, but v2's findRelevant subset requires false — two opposite flag
    // values, can't reuse a single method.
    virtual void seedActiveSubset(const std::vector<std::string>& names) = 0;

    // Reports whether the tool active set was seeded as the full pool
    // (progressive always; selective v2's 降级 / 全相关 paths; NOT a
    // findRelevant subset). tool_search.search uses this to short-circuit
    // (full pool => nothing left to discover beyond active) vs invoke real
    // recall (subset => search may find more). State-based, not mode-label-
    // based: the v1-selective-fallback and v2-selective share the SELECTIVE
    // label but differ in active-set state, so the label alone cannot
    // distinguish them — this runtime signal does.
    virtual bool isActiveFullPool() const = 0;

    // Seed skillActiveSet with the full skill pool. Sets
    // skillActiveIsFullPool_=true (drives skill_search.search short-circuit).
    // Symmetric with seedActive on the tool side. AgentWorker-level.
    virtual void seedSkillActive(const std::vector<std::string>& names) = 0;

    // Seed skillActiveSet with a findRelevant subset. Sets
    // skillActiveIsFullPool_=false (so skill_search.search routes to real
    // recall). Symmetric with seedActiveSubset on the tool side.
    virtual void seedSkillActiveSubset(const std::vector<std::string>& names) = 0;

    // Reports whether the skill active set was seeded as the full skill
    // pool. Symmetric with isActiveFullPool; drives skill_search.search
    // short-circuit (mirrors tool side §5.3 短路状态通道).
    virtual bool isActiveFullSkillPool() const = 0;
};

} // namespace jiuwen

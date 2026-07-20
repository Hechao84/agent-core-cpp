#pragma once

#include <set>
#include <string>
#include "include/tool.h"
#include "src/core/tool_turn_state.h"

namespace jiuwen {

// tool_search is the progressive-disclosure escape valve: it lets the model
// load a tool's full schema into the FC array (action=load) or discover
// relevant tool names when it has no name in hand (action=search). Mirrors
// skill_search's two-action shape but with a different load semantics:
// tool_search load injects the schema into SUBSEQUENT FC arrays (a calling
// contract that must persist), whereas skill_search load returns the body
// text into the current message (reference text, consumed and discarded).
//
// Registered as a session-scoped tool only when toolDisclosureMode is
// PROGRESSIVE/SELECTIVE (registration at SessionManager::Initialize /
// ReloadAgent). The tool instance is fresh per invocation
// (RegisterSessionTool registers a factory; CreateSessionTool calls it each
// time), receiving turnState via ToolBuildContext. The full alwaysOn set
// (MetaToolNames() ∪ config_.alwaysOnTools) is also injected at construction
// so the load action can idempotently short-circuit on ANY alwaysOn name,
// not just the meta-tools — alwaysOn is config-derived static state (not
// per-turn), so it does not belong on the ToolTurnState interface.
//
// The search action's short-circuit decision is driven by
// ToolTurnState::isActiveFullPool() (runtime state: active seeded as the
// full pool => nothing to discover => short-circuit), NOT by the mode label.
// This lets v1-selective-fallback (active = full pool) short-circuit
// correctly alongside progressive, while v2-selective (findRelevant subset)
// will route to real recall once findRelevant lands — without the tool
// branching on a mode label (which conflated v1-fallback and v2-selective).
class ToolSearchTool : public Tool {
public:
    // turnState may be nullptr in the null-ctx probe path (schema extraction
    // only); Invoke reports an error if turnState was never wired.
    // alwaysOnNames is the full alwaysOn set (meta-tools ∪ user-configured),
    // injected by SessionManager at registration so load can idempotently
    // short-circuit on any alwaysOn name. Default-empty for the probe path
    // and tests that don't exercise the idempotency branch.
    explicit ToolSearchTool(ToolTurnState* turnState = nullptr,
                            std::set<std::string> alwaysOnNames = {});
    std::string Invoke(const std::string& input) override;

private:
    ToolTurnState* turnState_;
    std::set<std::string> alwaysOnNames_;
};

} // namespace jiuwen

#include "src/core/tool_turn_state_proxy.h"

#include "include/session_manager.h"

namespace jiuwen {

void ToolTurnStateProxy::load(const std::string& name)
{
    // Name validation (ResourceManager::HasTool/HasSessionTool) is done by
    // the tool_search tool layer before calling this; here we just write to
    // the sets. Both insertions are idempotent (set semantics):
    //  - activeSet.insert: in progressive the name is already present
    //    (active = full pool); in selective it pulls in a name not yet
    //    active (preserving the §8 invariant 2 monotonicity).
    //  - loadedTools.insert: a re-load of an already-loaded name is a no-op.
    host_->activeSet.insert(name);
    host_->loadedTools.insert(name);
}

std::vector<std::string> ToolTurnStateProxy::search(const std::string& query)
{
    (void)query;
    // v1: real recall is not implemented. The tool_search tool short-circuits
    // in progressive mode before reaching here (returns the fixed "no recall
    // needed, please load directly" prompt), and selective v1 falls back to
    // progressive per §5.0. v2 will implement pool recall here (LLM-backed
    // findRelevant; the pool is reachable via SessionEntry->agent->worker_
    // or ResourceManager::GetAvailableTools).
    return {};
}

const std::set<std::string>& ToolTurnStateProxy::getActiveSet() const
{
    return host_->activeSet;
}

const std::set<std::string>& ToolTurnStateProxy::getLoadedTools() const
{
    return host_->loadedTools;
}

void ToolTurnStateProxy::reset()
{
    host_->activeSet.clear();
    host_->loadedTools.clear();
    activeIsFullPool_ = false;
}

void ToolTurnStateProxy::seedActive(const std::vector<std::string>& names)
{
    // Bulk-seed activeSet only (not loadedTools). Idempotent for names
    // already present. Used at Invoke entry to populate active = full pool
    // in progressive (and selective v1 fallback).
    for (const auto& n : names) {
        host_->activeSet.insert(n);
    }
    activeIsFullPool_ = true;
}

bool ToolTurnStateProxy::isActiveFullPool() const
{
    return activeIsFullPool_;
}

} // namespace jiuwen

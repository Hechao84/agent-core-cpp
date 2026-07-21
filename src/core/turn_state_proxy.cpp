#include "src/core/turn_state_proxy.h"

#include "include/session_manager.h"

namespace jiuwen {

void TurnStateProxy::load(const std::string& name)
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

std::vector<std::string> TurnStateProxy::search(const std::string& query)
{
    (void)query;
    // v2's actual LLM-backed recall is invoked by tool_search_tool directly
    // via CapabilitySelector (not by this method). This stub returns empty
    // for interface completeness and for any code path that still calls
    // through TurnState::search (none in v2's production path, kept for
    // interface symmetry with skill side and for tests).
    return {};
}

const std::set<std::string>& TurnStateProxy::getActiveSet() const
{
    return host_->activeSet;
}

const std::set<std::string>& TurnStateProxy::getLoadedTools() const
{
    return host_->loadedTools;
}

std::vector<std::string> TurnStateProxy::searchSkill(const std::string& query)
{
    (void)query;
    // v2's actual LLM-backed recall is invoked by skill_search_tool directly
    // via CapabilitySelector (not by this method). Stub returns empty for
    // interface symmetry with the tool side's search().
    return {};
}

const std::set<std::string>& TurnStateProxy::getSkillActiveSet() const
{
    return host_->skillActiveSet;
}

void TurnStateProxy::reset()
{
    host_->activeSet.clear();
    host_->loadedTools.clear();
    host_->skillActiveSet.clear();
    activeIsFullPool_ = false;
    skillActiveIsFullPool_ = false;
}

void TurnStateProxy::seedActive(const std::vector<std::string>& names)
{
    // Bulk-seed activeSet only (not loadedTools). Idempotent for names
    // already present. Used at Invoke entry to populate active = full pool
    // in progressive (and selective v2's 降级 / 全相关 paths).
    for (const auto& n : names) {
        host_->activeSet.insert(n);
    }
    activeIsFullPool_ = true;
}

void TurnStateProxy::seedActiveSubset(const std::vector<std::string>& names)
{
    // Bulk-seed activeSet with a findRelevant subset. Idempotent for names
    // already present (std::set dedupes). Sets activeIsFullPool_=false so
    // tool_search.search routes to real recall (§5.4.1 条 1).
    // Does NOT touch loadedTools (findRelevant predicts relevance, not
    // necessity; pre-loading full schemas would waste context — §7 决策1).
    for (const auto& n : names) {
        host_->activeSet.insert(n);
    }
    activeIsFullPool_ = false;
}

bool TurnStateProxy::isActiveFullPool() const
{
    return activeIsFullPool_;
}

void TurnStateProxy::seedSkillActive(const std::vector<std::string>& names)
{
    // Symmetric with seedActive on the tool side. Bulk-seed skillActiveSet
    // with the full skill pool. Sets skillActiveIsFullPool_=true.
    for (const auto& n : names) {
        host_->skillActiveSet.insert(n);
    }
    skillActiveIsFullPool_ = true;
}

void TurnStateProxy::seedSkillActiveSubset(const std::vector<std::string>& names)
{
    // Symmetric with seedActiveSubset on the tool side. Bulk-seed
    // skillActiveSet with a findRelevant subset. Sets
    // skillActiveIsFullPool_=false so skill_search.search routes to real
    // recall.
    for (const auto& n : names) {
        host_->skillActiveSet.insert(n);
    }
    skillActiveIsFullPool_ = false;
}

bool TurnStateProxy::isActiveFullSkillPool() const
{
    return skillActiveIsFullPool_;
}

} // namespace jiuwen

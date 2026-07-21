#pragma once

#include <set>
#include <string>
#include <vector>

#include "src/core/turn_state.h"

namespace jiuwen {

struct SessionEntry;

// Concrete TurnState implementation that forwards load/search/getActiveSet/
// getLoadedTools/searchSkill/getSkillActiveSet to a SessionEntry's
// activeSet/loadedTools/skillActiveSet fields. Lazily constructed and owned
// by SessionEntry (std::unique_ptr<TurnState> turnStateProxy) via
// SmWorkerEnv::GetCurrentTurnState(). The raw SessionEntry* back-pointer is
// safe because the proxy's lifetime is bounded by SessionEntry's (the
// unique_ptr member is destroyed when SessionEntry is destroyed).
//
// Per-turn reset clears activeSet/loadedTools/skillActiveSet CONTENTS (at
// Invoke entry); the proxy OBJECT is not rebuilt across turns — it is
// constructed once per SessionEntry and stays stable, so the TurnState*
// handed to tool_search/skill_search instances stays valid for the session.
//
// V2 design notes (round5 §5.4.1 条 4):
// - activeIsFullPool_ flag semantics: "active was seeded as the full pool"
//   (set true by seedActive, false by seedActiveSubset/reset). NOT "active
//   size == pool size" — that would mis-fire when load() pulls a pool-
//   external name into activeSet. The flag is driven by the seed method,
//   not by inspecting active contents.
// - skillActiveIsFullPool_ mirrors the tool side for skill_search's
//   short-circuit (symmetric per §5.4.1 条 11).
class TurnStateProxy : public TurnState {
public:
    explicit TurnStateProxy(SessionEntry* host) : host_(host) {}

    void load(const std::string& name) override;
    std::vector<std::string> search(const std::string& query) override;
    const std::set<std::string>& getActiveSet() const override;
    const std::set<std::string>& getLoadedTools() const override;

    std::vector<std::string> searchSkill(const std::string& query) override;
    const std::set<std::string>& getSkillActiveSet() const override;

    void reset() override;
    void seedActive(const std::vector<std::string>& names) override;
    void seedActiveSubset(const std::vector<std::string>& names) override;
    bool isActiveFullPool() const override;
    void seedSkillActive(const std::vector<std::string>& names) override;
    void seedSkillActiveSubset(const std::vector<std::string>& names) override;
    bool isActiveFullSkillPool() const override;

private:
    SessionEntry* host_;
    // Tracks whether the tool active set was seeded as the full pool
    // (seedActive) vs a findRelevant subset (seedActiveSubset). Drives
    // isActiveFullPool() so tool_search.search short-circuits when there is
    // nothing to discover beyond active. seedActive() sets true;
    // seedActiveSubset() and reset() set false. An empty pool seeded via
    // seedActive still counts as "full pool" — recall is pointless either way.
    bool activeIsFullPool_{false};
    // Symmetric flag for the skill side. Drives isActiveFullSkillPool() so
    // skill_search.search short-circuits when skill active = full pool.
    bool skillActiveIsFullPool_{false};
};

} // namespace jiuwen

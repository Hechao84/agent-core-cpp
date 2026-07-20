#pragma once

#include <set>
#include <string>
#include <vector>

#include "src/core/tool_turn_state.h"

namespace jiuwen {

struct SessionEntry;

// Concrete ToolTurnState implementation that forwards load/search/
// getActiveSet/getLoadedTools to a SessionEntry's activeSet/loadedTools
// fields. Lazily constructed and owned by SessionEntry
// (std::unique_ptr<ToolTurnState> turnStateProxy) via
// SmWorkerEnv::GetCurrentTurnState(). The raw SessionEntry* back-pointer is
// safe because the proxy's lifetime is bounded by SessionEntry's (the
// unique_ptr member is destroyed when SessionEntry is destroyed).
//
// Per-turn reset clears activeSet/loadedTools CONTENTS (at Invoke entry);
// the proxy OBJECT is not rebuilt across turns — it is constructed once per
// SessionEntry and stays stable, so the ToolTurnState* handed to tool_search
// instances stays valid for the session.
class ToolTurnStateProxy : public ToolTurnState {
public:
    explicit ToolTurnStateProxy(SessionEntry* host) : host_(host) {}

    void load(const std::string& name) override;
    std::vector<std::string> search(const std::string& query) override;
    const std::set<std::string>& getActiveSet() const override;
    const std::set<std::string>& getLoadedTools() const override;
    void reset() override;
    void seedActive(const std::vector<std::string>& names) override;
    bool isActiveFullPool() const override;

private:
    SessionEntry* host_;
    // Tracks whether the active set was seeded as the full pool (seedActive)
    // vs a findRelevant subset (v2, not yet implemented). Drives
    // isActiveFullPool() so tool_search.search short-circuits when there is
    // nothing to discover beyond active. seedActive() sets true; reset()
    // clears to false (next turn re-seeds). An empty pool seeded via
    // seedActive still counts as "full pool" — recall is pointless either way.
    bool activeIsFullPool_{false};
};

} // namespace jiuwen

#include "src/tools/builtin_tools/tool_search_tool.h"
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "include/resource_manager.h"
#include "src/core/capability_selector.h"
#include "src/utils/logger.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

ToolSearchTool::ToolSearchTool(TurnState* turnState,
                                std::set<std::string> alwaysOnNames,
                                CapabilitySelector* capabilitySelector)
    : Tool("tool_search",
           "Discover and load tools by name. Use action='load' when you already know the tool name (pulls its full schema into the callable set for the next iteration); use action='search' with a query when you only have a need and no name in hand. When all tools are already visible in the catalog, search short-circuits (nothing to discover).",
           {
               ToolParam{"action", "Action: 'load' (enable a known tool) or 'search' (find tools by query)", "string", true},
               ToolParam{"query", "Tool name (for load) or search query (for search)", "string", true},
           }),
      turnState_(turnState),
      alwaysOnNames_(std::move(alwaysOnNames)),
      capabilitySelector_(capabilitySelector)
{
}

std::string ToolSearchTool::Invoke(const std::string& input)
{
    if (!turnState_) {
        return "Error: tool_search turn state not initialized.";
    }

    std::string action;
    std::string query;

    try {
        nlohmann::json j = nlohmann::json::parse(input);
        action = j.value("action", "");
        query = j.value("query", "");
    } catch (...) {
        return "Error: Invalid JSON input. Expected: {\"action\": \"load|search\", \"query\": \"...\"}";
    }

    if (query.empty()) {
        return "Error: 'query' parameter is required.";
    }

    if (action == "load") {
        // Validate the name is a registered tool (stateless or session-
        // scoped). Mirrors AgentWorker::AddTools' HasTool||HasSessionTool
        // pattern. Unknown names return an error rather than being silently
        // dropped into loadedTools (§5.3 越界处理: pool-external → "未找到").
        auto& rm = ResourceManager::GetInstance();
        if (!rm.HasTool(query) && !rm.HasSessionTool(query)) {
            return "Error: Tool '" + query + "' not found in the tool pool.";
        }
        // Meta-tool idempotency: loading a name already in the alwaysOn set
        // is a no-op — the name is already in FC via alwaysOn (the FC
        // construction in BuildToolSchemas is alwaysOn ∪ loadedTools, set
        // semantics dedupes; writing to loadedTools would not change the
        // next-iteration schema). The full alwaysOn set (meta-tools ∪
        // config_.alwaysOnTools) is injected at construction by
        // SessionManager, so this short-circuits on ANY alwaysOn name —
        // including user-configured ones — not just the meta-tools. This
        // matches §5.3 of the design doc: "alwaysOn tools (including
        // tool_search/skill_search themselves) loaded → idempotent".
        if (alwaysOnNames_.count(query) > 0) {
            return "Tool '" + query + "' is already in the FC (alwaysOn).";
        }
        // load() writes name to both activeSet (preserving §8 invariant 2
        // monotonicity for selective 越界) and loadedTools (so the next
        // BuildToolSchemas iteration includes it in FC). Idempotent for names
        // already present (set semantics).
        turnState_->load(query);
        return "Loaded tool '" + query + "'; it will be callable from the next iteration.";
    }

    if (action == "search") {
        // Short-circuit decision is driven by TurnState::isActiveFullPool()
        // (runtime state), not the mode label. When the active set was seeded
        // as the full pool (progressive always; selective v1 fallback too),
        // there is nothing to discover beyond active → return the fixed "no
        // recall needed" prompt. When active is a findRelevant subset (v2),
        // route to TurnState::search() for real recall. This avoids the
        // v1-selective-fallback / v2-selective label collision (same SELECTIVE
        // mode, different active-set state) that a mode-label branch would
        // conflate — see the proxy's isActiveFullPool() for the state signal.
        //
        // The short-circuit lives in the tool layer (not inside
        // TurnState::search): the interface returns vector<string>
        // (names), incompatible with the fixed "no recall needed" prompt
        // string, so the tool decides before invoking the interface.
        if (turnState_->isActiveFullPool()) {
            return "No recall needed (all tools are already visible in the catalog). Use action='load' directly.";
        }
        // V2 real-recall branch (round5 §5.4.1 条 8/9). The actual LLM-backed
        // recall is invoked via CapabilitySelector (not via TurnState::search
        // — that remains a stub for interface symmetry). Turn-mid search
        // query is already context-aware (model writes it with full prompt
        // context), so pass empty sessionContext.
        if (capabilitySelector_ == nullptr) {
            // No selector wired (null-ctx probe path or a misconfiguration).
            // Match V1's "No tools found" response so the tool still returns
            // something usable rather than crashing.
            return "No tools found for query: " + query;
        }
        auto selection = capabilitySelector_->findRelevant(query, {});
        // Filter out names already in activeSet (dedup against existing).
        std::vector<std::string> newTools;
        const auto& active = turnState_->getActiveSet();
        for (const auto& t : selection.tools) {
            if (active.find(t) == active.end()) {
                newTools.push_back(t);
            }
        }
        if (newTools.empty()) {
            return "No tools found for query: " + query;
        }
        // Merge into active set via seedActiveSubset (preserves monotonicity;
        // flag is already false in this branch).
        turnState_->seedActiveSubset(newTools);
        std::string result = "Matching tools:\n";
        for (const auto& n : newTools) {
            result += "- " + n + "\n";
        }
        result += "\nUse action='load' with the tool name to enable it.";
        return result;
    }

    return "Error: Invalid action '" + action + "'. Use 'load' or 'search'.";
}

} // namespace jiuwen

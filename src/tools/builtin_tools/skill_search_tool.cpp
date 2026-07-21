#include "src/tools/builtin_tools/skill_search_tool.h"
#include <string>
#include <vector>
#include "src/core/capability_selector.h"
#include "src/skills/skill_engine.h"
#include "src/utils/logger.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

SkillSearchTool::SkillSearchTool(SkillEngine* engine, TurnState* turnState,
                                  CapabilitySelector* capabilitySelector)
    : Tool("skill_search", "Search for available skills and load their full instructions. Use when you need detailed guidance on a specific domain or task. Accepts JSON input with 'action' (search|load) and 'query' (search term or skill name).", {
    ToolParam{"action", "Action to perform: 'search' to find skills, 'load' to get full instructions", "string", true},
    ToolParam{"query", "Search query or skill name", "string", true}
}),
    engine_(engine),
    turnState_(turnState),
    capabilitySelector_(capabilitySelector)
{
}

std::string SkillSearchTool::Invoke(const std::string& input)
{
    std::string action;
    std::string query;

    try {
        nlohmann::json j = nlohmann::json::parse(input);
        action = j.value("action", "");
        query = j.value("query", "");
    } catch (...) {
        return "Error: Invalid JSON input. Expected: {\"action\": \"search|load\", \"query\": \"...\"}";
    }

    if (query.empty()) {
        return "Error: 'query' parameter is required.";
    }

    if (action == "search") {
        // V2 (round5 §5.4.1 条 11): short-circuit on isActiveFullSkillPool()
        // when TurnState is available. Mirrors tool_search's short-circuit
        // channel (§5.3). The short-circuit lives in the tool layer (not in
        // TurnState::searchSkill) because the interface returns vector<string>
        // (names), incompatible with the fixed "no recall needed" prompt.
        // Note: the engine_ null check is deferred to the load action and
        // the substring fallback — the short-circuit path doesn't need
        // engine_ (it just returns the fixed prompt).
        if (turnState_ != nullptr && turnState_->isActiveFullSkillPool()) {
            return "No recall needed (all skills are already visible in the catalog). Use action='load' directly.";
        }
        // Real recall path. V2 LLM-backed recall via CapabilitySelector
        // (round5 §5.4.1 条 8: search and findRelevant prefer the same LLM
        // backend; 条 9: reuse main modelConfig). When capabilitySelector_
        // is null (null-ctx probe path or disabled-mode stateless
        // registration that didn't get a selector), fall back to V1
        // substring matching so skill_search remains functional.
        if (capabilitySelector_ != nullptr && turnState_ != nullptr) {
            // Turn-mid search query is already context-aware (model writes
            // it with full prompt context), so pass empty sessionContext.
            auto selection = capabilitySelector_->findRelevant(query, {});
            std::vector<std::string> newSkills;
            const auto& active = turnState_->getSkillActiveSet();
            for (const auto& s : selection.skills) {
                if (active.find(s) == active.end()) {
                    newSkills.push_back(s);
                }
            }
            if (newSkills.empty()) {
                return "No skills found for query: " + query;
            }
            // Merge into active set via seedSkillActiveSubset (preserves
            // monotonicity; flag is already false in the real-recall path).
            turnState_->seedSkillActiveSubset(newSkills);
            std::string result = "Matching skills:\n";
            for (const auto& name : newSkills) {
                result += "- " + name + "\n";
            }
            result += "\nUse action='load' with the skill name to get full instructions.";
            return result;
        }
        // V1 fallback: substring matching via SkillEngine::SearchSkills.
        // Requires engine_ — if it's null, the tool was constructed in the
        // null-ctx probe path or under a misconfig; report the error rather
        // than crashing.
        if (engine_ == nullptr) {
            return "Error: SkillEngine not initialized.";
        }
        auto matches = engine_->SearchSkills(query);
        if (matches.empty()) {
            return "No skills found for query: " + query;
        }
        std::string result = "Matching skills:\n";
        for (const auto& name : matches) {
            result += "- " + name + "\n";
        }
        result += "\nUse action='load' with the skill name to get full instructions.";
        return result;
    } else if (action == "load") {
        // V1 unchanged (§5.4.1 条 11: skill Tier2 is stateless one-shot
        // GetSkillInstructions, no delayed state like tool's loadedTools).
        if (engine_ == nullptr) {
            return "Error: SkillEngine not initialized.";
        }
        return engine_->GetSkillInstructions(query);
    } else {
        return "Error: Invalid action '" + action + "'. Use 'search' or 'load'.";
    }
}

} // namespace jiuwen

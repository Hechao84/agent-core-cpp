
#include "src/tools/builtin_tools/skill_search_tool.h"
#include <string>
#include "src/3rd-party/include/nlohmann/json.hpp"
#include "src/skills/skill_engine.h"

SkillEngine* SkillSearchTool::engine_ = nullptr;

SkillSearchTool::SkillSearchTool() : Tool("skill_search", "Search for available skills and load their full instructions. Use when you need detailed guidance on a specific domain or task. Accepts JSON input with 'action' (search|load) and 'query' (search term or skill name).", {
    ToolParam{"action", "Action to perform: 'search' to find skills, 'load' to get full instructions", "string", true},
    ToolParam{"query", "Search query or skill name", "string", true}
})
{
}

void SkillSearchTool::SetEngine(SkillEngine* engine)
{
    engine_ = engine;
}

std::string SkillSearchTool::Invoke(const std::string& input)
{
    if (!engine_) {
        return "Error: SkillEngine not initialized.";
    }

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
        return engine_->GetSkillInstructions(query);
    } else {
        return "Error: Invalid action '" + action + "'. Use 'search' or 'load'.";
    }
}

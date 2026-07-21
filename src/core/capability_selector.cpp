#include "src/core/capability_selector.h"

#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "include/resource_manager.h"
#include "src/skills/skill_engine.h"
#include "src/utils/logger.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

namespace {

// Maximum number of items to recall on each side. The LLM is asked to pick
// the most relevant ones; capping the count keeps the Tier 1 catalog small
// enough to fit context under large tool/skill pools.
constexpr int kMaxTools = 8;
constexpr int kMaxSkills = 5;

} // namespace

CapabilitySelector::CapabilitySelector(AgentConfig config, SkillEngine* skillEngine)
    : config_(std::move(config)), skillEngine_(skillEngine)
{
}

CapabilitySelection CapabilitySelector::findRelevant(const std::string& rawQuery,
                                                     const std::vector<Message>& sessionContext)
{
    std::string prompt = BuildRecallPrompt(rawQuery, sessionContext);

    std::unique_ptr<Model> model;
    try {
        model = ResourceManager::GetInstance().CreateModel(config_.modelConfig);
    } catch (const std::exception& e) {
        LOG(ERR) << "[CapabilitySelector] Failed to create model: " << e.what();
        return {};
    }
    if (!model) {
        return {};
    }

    // Single user message carrying the recall prompt. No tools, no system
    // message — the prompt is self-contained.
    Message userMsg;
    userMsg.role = "user";
    userMsg.content = prompt;
    ModelResponse resp;
    try {
        resp = model->Invoke(model->Format("You are a capability recall assistant.",
                                          {userMsg}, {}),
                             nullptr);
    } catch (const std::exception& e) {
        LOG(ERR) << "[CapabilitySelector] Model invoke failed: " << e.what();
        return {};
    }
    if (resp.finishReason == "error" || resp.content.empty()) {
        LOG(WARN) << "[CapabilitySelector] Model returned error/empty: finishReason="
                  << resp.finishReason << ", content_len=" << resp.content.length();
        return {};
    }
    return ParseRecallResponse(resp.content);
}

std::string CapabilitySelector::BuildRecallPrompt(const std::string& rawQuery,
                                                  const std::vector<Message>& sessionContext) const
{
    auto& rm = ResourceManager::GetInstance();
    std::vector<std::string> allTools = rm.GetAvailableTools();
    std::set<std::string> visible(allTools.begin(), allTools.end());
    std::set<std::string> callable;  // empty — recall doesn't care about loadedTools
    std::string toolCatalog = rm.GetToolCatalog(visible,
        ResourceManager::CatalogRenderMode::NativeFc, callable);

    std::string skillCatalog;
    if (skillEngine_ != nullptr) {
        skillCatalog = skillEngine_->GetSkillCatalog();
    }

    std::ostringstream oss;
    oss << "You are a capability recall assistant. Given the user query and the conversation context, "
        << "select the most relevant tools and skills from the catalogs below.\n\n";
    oss << "## Available Tools\n" << toolCatalog << "\n\n";
    if (!skillCatalog.empty()) {
        oss << "## Available Skills\n" << skillCatalog << "\n\n";
    }
    if (!sessionContext.empty()) {
        oss << "## Conversation Context (most recent first)\n";
        int shown = 0;
        for (auto it = sessionContext.rbegin(); it != sessionContext.rend() && shown < 6; ++it, ++shown) {
            oss << "[" << it->role << "] " << it->content << "\n";
        }
        oss << "\n";
    }
    oss << "## User Query\n" << rawQuery << "\n\n";
    oss << "## Output Format\n"
        << "Return ONLY a JSON object, no prose, no markdown fences:\n"
        << "{\"tools\": [\"tool_name1\", \"tool_name2\", ...], \"skills\": [\"skill_name1\", ...]}\n"
        << "Rules:\n"
        << "- Include only tool/skill names that exist in the catalogs above.\n"
        << "- Select only capabilities highly relevant to the query (and context if provided).\n"
        << "- Return at most " << kMaxTools << " tools and " << kMaxSkills << " skills.\n"
        << "- If nothing is relevant, return {\"tools\": [], \"skills\": []}.\n";
    return oss.str();
}

CapabilitySelection CapabilitySelector::ParseRecallResponse(const std::string& response)
{
    // The model may wrap output in markdown fences or pre/post prose. Find
    // the first '{' and the matching last '}' to extract the JSON object.
    size_t braceStart = response.find('{');
    if (braceStart == std::string::npos) {
        return {};
    }
    size_t braceEnd = response.rfind('}');
    if (braceEnd == std::string::npos || braceEnd <= braceStart) {
        return {};
    }
    std::string jsonText = response.substr(braceStart, braceEnd - braceStart + 1);

    try {
        auto j = nlohmann::json::parse(jsonText);
        CapabilitySelection result;
        if (j.contains("tools") && j["tools"].is_array()) {
            for (const auto& t : j["tools"]) {
                if (t.is_string()) {
                    result.tools.push_back(t.get<std::string>());
                }
            }
        }
        if (j.contains("skills") && j["skills"].is_array()) {
            for (const auto& s : j["skills"]) {
                if (s.is_string()) {
                    result.skills.push_back(s.get<std::string>());
                }
            }
        }
        return result;
    } catch (const std::exception& e) {
        LOG(WARN) << "[CapabilitySelector] JSON parse failed: " << e.what()
                  << ", response=" << response;
        return {};
    }
}

} // namespace jiuwen

#pragma once

#include <set>
#include <string>
#include "include/tool.h"
#include "src/core/turn_state.h"

namespace jiuwen {

class SkillEngine;
class CapabilitySelector;

// SkillSearchTool is a session-scoped tool: SkillEngine is injected via
// ToolBuildContext (ctx.skillEngine) at construction time, so each instance
// binds to the active Agent's SkillEngine. This avoids the global static
// pointer + ReloadAgent timing fragility of the previous SetEngine pattern.
//
// V2 (round5 §5.4.1 条 11): search action's short-circuit decision is driven
// by TurnState::isActiveFullSkillPool() (runtime state), mirroring tool_search's
// isActiveFullPool signal. When active is the full skill pool (progressive
// always; selective v2 全相关 / 降级), short-circuit returns "No recall
// needed". When active is a findRelevant subset, route to CapabilitySelector
// for real LLM recall.
//
// load action is unchanged from V1 (skill Tier2 is stateless one-shot
// GetSkillInstructions, no delayed state — §5.4.1 条 11 "tool/skill load
// 不对称"). V2 does NOT add loadSkill/getLoadedSkills to TurnState for
// skills.
class SkillSearchTool : public Tool {
public:
    // engine may be nullptr in the null-ctx probe path (schema extraction
    // only); Invoke reports an error if engine was never wired.
    // turnState and capabilitySelector are optional for backward
    // compatibility with the null-ctx probe path; under disabled mode
    // (stateless registration via RegisterBuiltinTools), ExecuteTool still
    // fills them but the search action falls back to substring matching
    // when capabilitySelector_ is null.
    explicit SkillSearchTool(SkillEngine* engine = nullptr,
                              TurnState* turnState = nullptr,
                              CapabilitySelector* capabilitySelector = nullptr);
    std::string Invoke(const std::string& input) override;

private:
    SkillEngine* engine_;
    TurnState* turnState_;
    CapabilitySelector* capabilitySelector_;
};

} // namespace jiuwen

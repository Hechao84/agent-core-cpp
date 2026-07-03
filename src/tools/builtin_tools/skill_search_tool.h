#pragma once

#include <string>
#include "include/tool.h"

namespace jiuwen {

class SkillEngine;

// SkillSearchTool is a session-scoped tool: SkillEngine is injected via
// ToolBuildContext (ctx.skillEngine) at construction time, so each instance
// binds to the active Agent's SkillEngine. This avoids the global static
// pointer + ReloadAgent timing fragility of the previous SetEngine pattern.
class SkillSearchTool : public Tool {
public:
    // engine may be nullptr in the null-ctx probe path (schema extraction
    // only); Invoke reports an error if engine was never wired.
    explicit SkillSearchTool(SkillEngine* engine = nullptr);
    std::string Invoke(const std::string& input) override;

private:
    SkillEngine* engine_;
};

} // namespace jiuwen

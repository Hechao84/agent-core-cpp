#pragma once

#include <string>
#include "include/tool.h"

class SkillEngine;

class SkillSearchTool : public Tool {
public:
    SkillSearchTool();
    std::string Invoke(const std::string& input) override;

    // Set the global SkillEngine instance for this tool to use
    static void SetEngine(SkillEngine* engine);

private:
    static SkillEngine* engine_;
};

#pragma once

#include <string>

#include "include/agent_export.h"

namespace jiuwen {

// Public POD describing one loaded skill. The full lifecycle (discovery,
// caching, on-demand body load) is owned by the framework-internal
// SkillEngine; this struct is the stable shape exposed to applications
// (e.g. UIs, HTTP adapters) via Agent::ListSkills / Agent::GetSkill.
struct AGENT_API Skill
{
    std::string id;          // Directory name (also unique key)
    std::string name;        // Extracted from YAML frontmatter
    std::string description; // Extracted from YAML frontmatter
    std::string body;        // Full SKILL.md body (after frontmatter)
    std::string directory;   // Full path to skill directory
};

} // namespace jiuwen

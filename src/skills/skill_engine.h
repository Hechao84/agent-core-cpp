#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace jiuwen {

struct Skill {
    std::string id;          // Directory name
    std::string name;        // Extracted from YAML frontmatter
    std::string description; // Extracted from YAML frontmatter
    std::string body;        // Full SKILL.md body (after frontmatter)
    std::string directory;   // Full path to skill directory
};

// SkillLevel controls progressive disclosure depth
enum class SkillLevel {
    Metadata,    // name + description only (always in prompt)
    FullBody     // name + description + full SKILL.md instructions
};

// SkillEngine handles loading and managing skills from a directory structure
// with progressive disclosure: only metadata is in system prompt by default,
// full instructions are loaded on demand via the skill_search tool.
class SkillEngine {
public:
    SkillEngine() = default;
    explicit SkillEngine(const std::string& rootDir);
    void SetRootDir(const std::string& rootDir);

    // Load skills from disk, parsing metadata from YAML frontmatter.
    // Reloads if forceReload is true.
    bool Load(bool forceReload = false);

    // Level 1: Get compact metadata list for system prompt
    // Format: "- <name>: <description>" per skill
    std::string GetSkillCatalog() const;

    // Level 2: Get full instructions for a specific skill by name
    // Returns empty string if skill not found
    std::string GetSkillInstructions(const std::string& skillName) const;

    // Search skills by query matching name or description
    // Returns list of matching skill names
    std::vector<std::string> SearchSkills(const std::string& query) const;

    // Get list of all skill IDs
    std::vector<std::string> GetSkillIds() const;

private:
    std::string rootDir_;
    std::unordered_map<std::string, Skill> skills_;
    std::string lastMtime_;

    Skill ParseSkillDir(const std::string& dirPath, const std::string& folderName) const;
    std::string ExtractFrontmatterField(const std::string& content, const std::string& key) const;
};

} // namespace jiuwen

#pragma once

#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "include/skill.h"

namespace jiuwen {

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
    // V2 (round5 §5.4.1 条 11): the no-arg overload renders the full pool
    // (used in DISABLED mode — current V1 behavior, zero regression).
    std::string GetSkillCatalog() const;

    // V2 by-subset overload (round5 §5.4.1 条 11): render only the skills
    // whose name appears in visibleSkillNames. Mirrors
    // ResourceManager::GetToolCatalog(visibleNames, ...) — caller computes
    // visibleSkillNames = skillActiveSet and passes it here. SkillEngine stays
    // Agent-scoped (no WorkerEnv dependency); the caller (AgentWorker::
    // BuildPrompt) reads the active set via TLS through
    // WorkerEnv::GetCurrentTurnState() and passes the resolved names here.
    //
    // Note: skill side has NO alwaysOn concept — visible = skillActive only
    // (no union with tool-side ComputeAlwaysOn()). Rationale: tools need
    // alwaysOn because the FC protocol hard-restricts calls to the FC array
    // (escape valves must always be callable under SELECTIVE). Skills have no
    // protocol gate — they're prompt text references, not callable endpoints.
    // The escape valve for skills IS skill_search (a tool, alwaysOn
    // meta-tool, belongs to the tool-side alwaysOn set); the model uses it
    // to discover/load skills. So "skill alwaysOn" would be a concept without
    // structural purpose. Extends §5.4.1 条 11's "tool/skill load 不对称" to
    // the alwaysOn dimension: tools need it (FC gate bypass), skills don't.
    std::string GetSkillCatalog(const std::set<std::string>& visibleSkillNames) const;

    // Level 2: Get full instructions for a specific skill by name
    // Returns empty string if skill not found
    std::string GetSkillInstructions(const std::string& skillName) const;

    // Search skills by query matching name or description
    // Returns list of matching skill names
    std::vector<std::string> SearchSkills(const std::string& query) const;

    // Get list of all skill IDs
    std::vector<std::string> GetSkillIds() const;

    // Get all loaded skills (metadata + body); ordered by id.
    std::vector<Skill> GetAllSkills() const;

    // Get one skill by id; returns empty Skill if not found.
    Skill GetSkill(const std::string& id) const;

    // Configured root directory (for diagnostics).
    std::string GetRootDir() const;

private:
    std::string rootDir_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Skill> skills_;
    std::string lastMtime_;

    Skill ParseSkillDir(const std::string& dirPath, const std::string& folderName) const;
    std::string ExtractFrontmatterField(const std::string& content, const std::string& key) const;
};

} // namespace jiuwen

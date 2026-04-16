#pragma once

#include <string>
#include <unordered_map>
#include <vector>

struct Skill {
    std::string id; // Directory name
    std::string name; // Skill Name (extracted from header or folder name)
    std::string description;
    std::string instructions; // Full markdown content
};

// SkillEngine handles loading and managing skills from a directory structure
class SkillEngine {
public:
    SkillEngine() = default;
    explicit SkillEngine(const std::string& rootDir);
    void SetRootDir(const std::string& rootDir);

    // Load skills from disk.
    // Note: For "Hot Update" support, this should be called before each usage
    // or triggered by a file watcher. Given Agent Invoke frequency,
    // we can afford to check directory modification times.
    bool Load(bool forceReload = false);

    // Get formatted content for prompt injection
    std::string GetFormattedInstructions() const;

    // Get list of skill IDs
    std::vector<std::string> GetSkillIds() const;

private:
    std::string rootDir_;
    std::unordered_map<std::string, Skill> skills_;

    Skill ParseSkillDir(const std::string& dirPath, const std::string& folderName) const;
};

#include "src/skills/skill_engine.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace jiuwen {

SkillEngine::SkillEngine(const std::string& rootDir) : rootDir_(rootDir)
{
}

void SkillEngine::SetRootDir(const std::string& rootDir)
{
    rootDir_ = rootDir;
}

bool SkillEngine::Load(bool forceReload)
{
    if (rootDir_.empty()) return false;
    if (!fs::exists(rootDir_) || !fs::is_directory(rootDir_)) {
        std::cerr << "SkillEngine: Root directory '" << rootDir_ << "' not found." << std::endl;
        return false;
    }

    // Check modification time to skip reloading if nothing changed
    if (!forceReload && !skills_.empty()) {
        try {
            std::string currentMtime = std::to_string(
                std::chrono::duration_cast<std::chrono::seconds>(
                    fs::last_write_time(rootDir_).time_since_epoch()).count());
            if (currentMtime == lastMtime_) {
                return true;
            }
            lastMtime_ = currentMtime;
        } catch (...) {
            // Ignore errors, proceed to load
        }
    }

    skills_.clear();

    for (const auto& entry : fs::directory_iterator(rootDir_)) {
        if (entry.is_directory()) {
            std::string folderName = entry.path().filename().string();
            Skill skill = ParseSkillDir(entry.path().string(), folderName);
            if (!skill.id.empty()) {
                skills_[skill.id] = skill;
            }
        }
    }

    return true;
}

Skill SkillEngine::ParseSkillDir(const std::string& dirPath, const std::string& folderName) const
{
    Skill skill;
    skill.id = folderName;
    skill.name = folderName;
    skill.description = "";
    skill.directory = dirPath;

    std::vector<std::string> candidates = {"SKILL.md", "skill.md", "CLAUDE.md"};
    for (const auto& candidate : candidates) {
        std::string testPath = (fs::path(dirPath) / candidate).string();
        if (fs::exists(testPath)) {
            try {
                std::ifstream file(testPath);
                std::stringstream buffer;
                buffer << file.rdbuf();
                std::string content = buffer.str();

                // Extract YAML frontmatter fields
                skill.name = ExtractFrontmatterField(content, "name");
                skill.description = ExtractFrontmatterField(content, "description");

                // If frontmatter didn't have name, use folder name
                if (skill.name.empty()) skill.name = folderName;

                // Extract body: everything after the closing --- of frontmatter
                size_t frontEnd = content.find("---\n");
                if (frontEnd != std::string::npos) {
                    frontEnd = content.find("---\n", frontEnd + 4);
                    if (frontEnd != std::string::npos) {
                        skill.body = content.substr(frontEnd + 4);
                    } else {
                        skill.body = content;
                    }
                } else {
                    skill.body = content;
                }

                return skill;
            } catch (const std::exception& e) {
                std::cerr << "Failed to read skill file: " << e.what() << std::endl;
            }
            break;
        }
    }

    skill.description = "No SKILL.md found.";
    skill.body = "# Skill " + folderName + "\nNo instructions found.";
    return skill;
}

std::string SkillEngine::ExtractFrontmatterField(const std::string& content, const std::string& key) const
{
    // Look for frontmatter: content between first --- and second ---
    size_t frontStart = content.find("---\n");
    if (frontStart == std::string::npos) return "";
    frontStart += 4;
    size_t frontEnd = content.find("---\n", frontStart);
    if (frontEnd == std::string::npos) return "";

    std::string frontmatter = content.substr(frontStart, frontEnd - frontStart);

    // Find key: value in frontmatter
    std::string searchKey = key + ":";
    size_t keyPos = frontmatter.find(searchKey);
    if (keyPos == std::string::npos) return "";

    // Extract value after colon
    size_t valStart = frontmatter.find_first_not_of(" \t", keyPos + searchKey.length());
    if (valStart == std::string::npos) return "";

    // Handle quoted values
    if (frontmatter[valStart] == '"') {
        size_t valEnd = frontmatter.find('"', valStart + 1);
        if (valEnd == std::string::npos) return "";
        return frontmatter.substr(valStart + 1, valEnd - valStart - 1);
    }

    // Handle unquoted value (until end of line)
    size_t valEnd = frontmatter.find('\n', valStart);
    if (valEnd == std::string::npos) {
        return frontmatter.substr(valStart);
    }

    std::string value = frontmatter.substr(valStart, valEnd - valStart);

    // Trim trailing whitespace
    size_t trimEnd = value.find_last_not_of(" \t\r\n");
    if (trimEnd != std::string::npos) {
        value = value.substr(0, trimEnd + 1);
    }
    return value;
}

std::string SkillEngine::GetSkillCatalog() const
{
    if (skills_.empty()) return "No skills available.";

    std::string result = "Available Skills (use skill_search tool to load full instructions):\n";
    for (const auto& [id, skill] : skills_) {
        result += "- " + skill.name + ": " + skill.description + "\n";
    }
    return result;
}

std::string SkillEngine::GetSkillInstructions(const std::string& skillName) const
{
    // Case-insensitive search by name
    for (const auto& [id, skill] : skills_) {
        std::string lowerName = skill.name;
        std::string lowerQuery = skillName;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);
        if (lowerName == lowerQuery) {
            std::string result = "## Skill: " + skill.name + "\n";
            result += skill.body;
            return result;
        }
    }
    return "Skill '" + skillName + "' not found.";
}

std::vector<std::string> SkillEngine::SearchSkills(const std::string& query) const
{
    std::vector<std::string> results;
    std::string lowerQuery = query;
    std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);

    // Split query into words
    std::vector<std::string> words;
    std::istringstream iss(lowerQuery);
    std::string word;
    while (iss >> word) {
        words.push_back(word);
    }
    if (words.empty()) return results;

    for (const auto& [id, skill] : skills_) {
        std::string lowerName = skill.name;
        std::string lowerDesc = skill.description;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        std::transform(lowerDesc.begin(), lowerDesc.end(), lowerDesc.begin(), ::tolower);

        int matchCount = 0;
        for (const auto& w : words) {
            if (lowerName.find(w) != std::string::npos) matchCount += 2;
            if (lowerDesc.find(w) != std::string::npos) matchCount += 1;
        }
        if (matchCount > 0) {
            results.push_back(skill.name);
        }
    }

    return results;
}

std::vector<std::string> SkillEngine::GetSkillIds() const
{
    std::vector<std::string> ids;
    ids.reserve(skills_.size());
    for (const auto& [id, _] : skills_) {
        ids.push_back(id);
    }
    return ids;
}

} // namespace jiuwen

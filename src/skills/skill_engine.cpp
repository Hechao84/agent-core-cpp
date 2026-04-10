#include "skills/skill_engine.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

namespace fs = std::filesystem;

SkillEngine::SkillEngine(const std::string& rootDir) : m_rootDir(rootDir) {}

void SkillEngine::SetRootDir(const std::string& rootDir) {
    m_rootDir = rootDir;
}

bool SkillEngine::Load(bool forceReload) {
    if (m_rootDir.empty()) return false;
    if (!fs::exists(m_rootDir) || !fs::is_directory(m_rootDir)) {
        std::cerr << "SkillEngine: Root directory '" << m_rootDir << "' not found." << std::endl;
        return false;
    }

    if (!forceReload) {
        // Optimization: Skip reloading if the directory hasn't been modified recently.
        // This provides a "Hot Update" mechanism without a full watcher service.
        try {
            auto lastWrite = fs::last_write_time(m_rootDir);
            // Simple heuristic: if we have skills loaded, maybe skip? 
            // But to be truly "Hot", checking mtime is good.
            // For C++17 filesystem, comparing file_time_type is sufficient.
            // However, simply reloading is safer and fast enough for this architecture.
        } catch (...) { /* Ignore errors, just proceed to load */ }
    }

    // Re-scan and Reload
    m_skills.clear();
    
    for (const auto& entry : fs::directory_iterator(m_rootDir)) {
        if (entry.is_directory()) {
            std::string folderName = entry.path().filename().string();
            Skill skill = ParseSkillDir(entry.path().string(), folderName);
            if (!skill.id.empty()) {
                m_skills[skill.id] = skill;
            }
        }
    }
    
    return true;
}

Skill SkillEngine::ParseSkillDir(const std::string& dirPath, const std::string& folderName) const {
    Skill skill;
    skill.id = folderName;
    skill.name = folderName; // Default name

    // Look for standard Anthropic skill files: skill.md, SKILL.md
    std::string filePath;
    std::vector<std::string> candidates = {"skill.md", "SKILL.md", "CLAUDE.md"};
    for (const auto& candidate : candidates) {
        std::string testPath = (fs::path(dirPath) / candidate).string();
        if (fs::exists(testPath)) {
            filePath = testPath;
            break;
        }
    }

    if (!filePath.empty()) {
        try {
            std::ifstream file(filePath);
            std::stringstream buffer;
            buffer << file.rdbuf();
            skill.instructions = buffer.str();
            
            // Attempt to extract Name and Description from Markdown
            // Simple regex-like parsing for headers
            size_t pos = skill.instructions.find("# ");
            if (pos != std::string::npos) {
                size_t end = skill.instructions.find("\n", pos);
                std::string line = skill.instructions.substr(pos, end - pos);
                // Remove "Name:" prefix if present
                size_t colPos = line.find(":");
                if (colPos != std::string::npos) {
                    skill.name = line.substr(colPos + 1);
                } else {
                    skill.name = line.substr(2);
                }
                // Trim whitespace
                skill.name.erase(0, skill.name.find_first_not_of(" \t\r\n"));
                skill.name.erase(skill.name.find_last_not_of(" \t\r\n") + 1);
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to read skill file: " << e.what() << std::endl;
        }
    } else {
        skill.instructions = "# Skill " + folderName + "\nNo instructions found.";
    }

    return skill;
}

std::string SkillEngine::GetFormattedInstructions() const {
    if (m_skills.empty()) return "";
    
    std::string result;
    result = "### Available Skills\n";
    
    // Append summary
    for (const auto& [id, skill] : m_skills) {
        result += "**" + skill.name + "**: " + skill.instructions.substr(0, 100) + "...\n";
    }
    
    result += "\n---\n\n### Detailed Skill Instructions\n";
    for (const auto& [id, skill] : m_skills) {
        result += skill.instructions + "\n\n---\n\n";
    }
    
    return result;
}

std::vector<std::string> SkillEngine::GetSkillIds() const {
    std::vector<std::string> ids;
    ids.reserve(m_skills.size());
    for (const auto& [id, _] : m_skills) {
        ids.push_back(id);
    }
    return ids;
}

#include "list_dir_tool.h"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <algorithm>
#include <set>

namespace fs = std::filesystem;

static const std::set<std::string> kIgnoreDirs = {
    ".git", "node_modules", "__pycache__", ".venv", "venv",
    "dist", "build", ".tox", ".mypy_cache", ".pytest_cache",
    ".ruff_cache", ".coverage", "htmlcov"
};
static const int kDefaultMax = 200;

static std::string ParseStringField(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";
    size_t colonPos = json.find(':', keyPos + searchKey.length());
    if (colonPos == std::string::npos) return "";
    size_t valStart = json.find_first_not_of(" \t", colonPos + 1);
    if (valStart == std::string::npos) return "";
    if (json[valStart] != '"') return "";
    size_t valEnd = valStart + 1;
    while (valEnd < json.length()) {
        if (json[valEnd] == '\\' && valEnd + 1 < json.length()) {
            valEnd += 2; continue;
        }
        if (json[valEnd] == '"') break;
        valEnd++;
    }
    return json.substr(valStart + 1, valEnd - valStart - 1);
}

static bool ParseBoolField(const std::string& json, const std::string& key, bool defaultVal) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return defaultVal;
    size_t colonPos = json.find(':', keyPos + searchKey.length());
    if (colonPos == std::string::npos) return defaultVal;
    size_t valStart = json.find_first_not_of(" \t", colonPos + 1);
    if (valStart == std::string::npos) return defaultVal;
    if (json.substr(valStart, 4) == "true") return true;
    if (json.substr(valStart, 5) == "false") return false;
    return defaultVal;
}

static int ParseIntField(const std::string& json, const std::string& key, int defaultVal) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return defaultVal;
    size_t colonPos = json.find(':', keyPos + searchKey.length());
    if (colonPos == std::string::npos) return defaultVal;
    size_t valStart = json.find_first_not_of(" \t", colonPos + 1);
    if (valStart == std::string::npos) return defaultVal;
    try { return std::stoi(json.substr(valStart)); } catch (...) { return defaultVal; }
}

ListDirTool::ListDirTool()
    : Tool("list_dir", "List directory contents. Supports recursive listing. "
         "Auto-ignores common noise dirs (.git, node_modules, etc.). "
         "Input: JSON with 'path' (required), 'recursive' (default false), "
         "'max_entries' (default 200).",
         {{"path", "The directory path to list", "string", true},
          {"recursive", "Recursively list all files", "boolean", false},
          {"max_entries", "Maximum entries to return", "integer", false}}) {}

std::string ListDirTool::Invoke(const std::string& input) {
    std::string dirPath = ParseStringField(input, "path");
    if (dirPath.empty()) {
        return "Error: 'path' parameter is required";
    }

    bool recursive = ParseBoolField(input, "recursive", false);
    int maxEntries = ParseIntField(input, "max_entries", kDefaultMax);

    fs::path dp(dirPath);
    if (!fs::exists(dp)) {
        return "Error: Directory not found: " + dirPath;
    }
    if (!fs::is_directory(dp)) {
        return "Error: Not a directory: " + dirPath;
    }

    std::vector<std::string> items;
    int total = 0;

    if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(dp)) {
            bool skip = false;
            for (const auto& part : entry.path()) {
                if (kIgnoreDirs.count(part.string())) {
                    skip = true;
                    break;
                }
            }
            if (skip) continue;
            total++;
            if (static_cast<int>(items.size()) < maxEntries) {
                auto rel = fs::relative(entry.path(), dp);
                std::string display = rel.generic_string();
                if (entry.is_directory()) display += "/";
                items.push_back(display);
            }
        }
    } else {
        for (const auto& entry : fs::directory_iterator(dp)) {
            if (kIgnoreDirs.count(entry.path().filename().string())) continue;
            total++;
            if (static_cast<int>(items.size()) < maxEntries) {
                if (entry.is_directory()) items.push_back("[DIR]  " + entry.path().filename().string());
                else items.push_back("[FILE] " + entry.path().filename().string());
            }
        }
    }

    std::sort(items.begin(), items.end());

    if (items.empty() && total == 0) {
        return "Directory " + dirPath + " is empty";
    }

    std::ostringstream oss;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) oss << "\n";
        oss << items[i];
    }
    if (total > maxEntries) {
        oss << "\n\n(truncated, showing first " << maxEntries << " of " << total << " entries)";
    }
    return oss.str();
}

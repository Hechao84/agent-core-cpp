

#include "src/tools/builtin_tools/glob_tool.h"
#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "filesystem"
#include "regex"

namespace fs = std::filesystem;

static const int kDefaultHeadLimit = 250;
static const std::set<std::string> kIgnoreDirs = {
    ".git", "node_modules", "__pycache__", ".venv", "venv",
    "dist", "build", ".tox", ".mypy_cache", ".pytest_cache"
};

static std::string ParseStringField(const std::string& json, const std::string& key)
{
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
            valEnd += 2; 
            continue; 
        }
        if (json[valEnd] == '"') {
            break;
        }
        valEnd++;
    }
    return json.substr(valStart + 1, valEnd - valStart - 1);
}

static int ParseIntField(const std::string& json, const std::string& key, int defaultVal)
{
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) {
        return defaultVal;
    }
    size_t colonPos = json.find(':', keyPos + searchKey.length());
    if (colonPos == std::string::npos) {
        return defaultVal;
    }
    size_t valStart = json.find_first_not_of(" \t", colonPos + 1);
    if (valStart == std::string::npos) {
        return defaultVal;
    }
    try { 
        return std::stoi(json.substr(valStart)); 
    } catch (...) { 
        return defaultVal; 
    }
}

static bool GlobMatch(const std::string& relPath, const std::string& fileName, const std::string& pattern)
{
    // Convert glob to regex
    std::string regexPattern;
    for (char c : pattern) {
        if (c == '*') regexPattern += ".*";
        else if (c == '?') regexPattern += ".";
        else if (c == '.') regexPattern += "\\.";
        else if (c == '[') regexPattern += "[";
        else if (c == ']') regexPattern += "]";
        else regexPattern += c;
    }
    regexPattern = "^" + regexPattern + "$";

    try {
        std::regex re(regexPattern, std::regex::icase);
        if (pattern.find('/') != std::string::npos || pattern.find("**") != std::string::npos) {
            return std::regex_match(relPath, re);
        }
        return std::regex_match(fileName, re);
    } catch (...) {
        return false;
    }
}

GlobTool::GlobTool()
    : Tool("glob", "Find files matching a glob pattern (e.g. '*.py', 'test_*.cpp'). "
         "Results sorted by modification time (newest first). "
         "Skips .git, node_modules, __pycache__, etc. "
         "Input: JSON with 'pattern' (required), 'path' (default '.'), "
         "'head_limit' (default 250), 'entry_type' (files|dirs|both, default files).",
         {{"pattern", "Glob pattern to match", "string", true},
          {"path", "Directory to search from", "string", false},
          {"head_limit", "Max results to return (default 250)", "integer", false},
          {"entry_type", "Match files, dirs, or both", "string", false}}){} std::string GlobTool::Invoke(const std::string& input)
{
    std::string pattern = ParseStringField(input, "pattern");
    if (pattern.empty()) {
        return "Error: 'pattern' parameter is required";
    }

    std::string searchPath = ParseStringField(input, "path");
    if (searchPath.empty()) searchPath = ".";

    int headLimit = ParseIntField(input, "head_limit", kDefaultHeadLimit);
    std::string entryType = ParseStringField(input, "entry_type");
    if (entryType.empty()) entryType = "files";
    if (headLimit <= 0) headLimit = kDefaultHeadLimit;

    fs::path root(searchPath);
    if (!fs::exists(root)) {
        return "Error: Path not found: " + searchPath;
    }
    if (!fs::is_directory(root)) {
        return "Error: Not a directory: " + searchPath;
    }

    bool includeFiles = (entryType == "files" || entryType == "both");
    bool includeDirs = (entryType == "dirs" || entryType == "both");

    std::vector<std::pair<std::string, double>> matches;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            bool skip = false;
            for (const auto& part : entry.path()) {
                if (kIgnoreDirs.count(part.string())) { 
                    skip = true; 
                    break; 
                }
            }
            if (skip) continue;

            bool matchType = false;
            if (includeFiles && entry.is_regular_file()) matchType = true;
            if (includeDirs && entry.is_directory()) matchType = true;
            if (!matchType) continue;

            std::string relPath = fs::relative(entry.path(), root).generic_string();
            if (GlobMatch(relPath, entry.path().filename().string(), pattern)) {
                std::string display = relPath;
                if (entry.is_directory()) display += "/";
                double mtime = 0.0;
                try { mtime = entry.last_write_time().time_since_epoch().count() / 1e9; } catch (...){} matches.push_back({display, mtime});
            }
        }
    } catch (const std::exception& e) {
        return "Error finding files: " + std::string(e.what());
    }

    if (matches.empty()) {
        return "No paths matched pattern '" + pattern + "' in " + searchPath;
    }

    // Sort by mtime (newest first), then by name
    std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b)
    {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });

    std::ostringstream oss;
    int count = 0;
    for (const auto& m : matches) {
        if (count >= headLimit) break;
        if (count > 0) oss << "\n";
        oss << m.first;
        count++;
    }
    if (static_cast<int>(matches.size()) > headLimit) {
        oss << "\n\n(pagination: limit=" << headLimit << ", offset=0)";
    }
    return oss.str();
}

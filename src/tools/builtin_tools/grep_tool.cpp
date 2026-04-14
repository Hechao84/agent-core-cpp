#include "grep_tool.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <set>
#include <regex>

namespace fs = std::filesystem;

static const int kDefaultHeadLimit = 250;
static const int kMaxResultChars = 128000;
static const int kMaxFileBytes = 2000000;
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
        if (json[valEnd] == '\\' && valEnd + 1 < json.length()) { valEnd += 2; continue; }
        if (json[valEnd] == '"') break;
        valEnd++;
    }
    return json.substr(valStart + 1, valEnd - valStart - 1);
}

static int ParseIntField(const std::string& json, const std::string& key, int defaultVal)
{
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return defaultVal;
    size_t colonPos = json.find(':', keyPos + searchKey.length());
    if (colonPos == std::string::npos) return defaultVal;
    size_t valStart = json.find_first_not_of(" \t", colonPos + 1);
    if (valStart == std::string::npos) return defaultVal;
    try { return std::stoi(json.substr(valStart)); } catch (...) { return defaultVal; }
}

static bool ParseBoolField(const std::string& json, const std::string& key, bool defaultVal)
{
    std::string val = ParseStringField(json, key);
    if (val == "true") return true;
    if (val == "false") return false;
    return defaultVal;
}

static bool IsBinary(const std::string& content)
{
    size_t sampleLen = std::min(content.length(), size_t(4096));
    if (sampleLen == 0) return false;
    int nonText = 0;
    for (size_t i = 0; i < sampleLen; ++i) {
        unsigned char c = static_cast<unsigned char>(content[i]);
        if (c == 0) return true;
        if ((c < 9 || (c > 13 && c < 32)) && c != 27) nonText++;
    }
    return (static_cast<double>(nonText) / sampleLen) > 0.2;
}

static bool GlobFilter(const std::string& fileName, const std::string& pattern)
{
    if (pattern.empty()) return true;
    // Simple glob match for *.ext patterns
    if (pattern.size() >= 2 && pattern[0] == '*' && pattern[1] == '.') {
        std::string ext = pattern.substr(1); // e.g., ".cpp"
        return fileName.size() >= ext.size() && fileName.substr(fileName.size() - ext.size()) == ext;
    }
    return true;
}

GrepTool::GrepTool()
    : Tool("grep", "Search file contents with a regex pattern. "
         "Default output: matching file paths. "
         "Supports content mode (with context), count mode, glob/type filtering. "
         "Skips binary and files >2MB. "
         "Input: JSON with 'pattern' (required), 'path' (default '.'), "
         "'output_mode' (files_with_matches|content|count), "
         "'case_insensitive' (default false), 'glob' (file filter), "
         "'head_limit' (default 250), 'context_before'/'context_after' (0-20).",
         {{"pattern", "Regex or plain text pattern", "string", true},
          {"path", "File or directory to search in", "string", false},
          {"output_mode", "Output mode: files_with_matches, content, or count", "string", false},
          {"case_insensitive", "Case-insensitive search", "boolean", false},
          {"glob", "Optional file filter, e.g. '*.cpp'", "string", false},
          {"head_limit", "Max results to return", "integer", false},
          {"context_before", "Lines of context before match", "integer", false},
          {"context_after", "Lines of context after match", "integer", false}}) {}

std::string GrepTool::Invoke(const std::string& input)
{
    std::string pattern = ParseStringField(input, "pattern");
    if (pattern.empty()) {
        return "Error: 'pattern' parameter is required";
    }

    std::string searchPath = ParseStringField(input, "path");
    if (searchPath.empty()) searchPath = ".";

    std::string outputMode = ParseStringField(input, "output_mode");
    if (outputMode.empty()) outputMode = "files_with_matches";

    std::string globFilter = ParseStringField(input, "glob");
    bool caseInsensitive = ParseBoolField(input, "case_insensitive", false);
    int headLimit = ParseIntField(input, "head_limit", kDefaultHeadLimit);
    int contextBefore = ParseIntField(input, "context_before", 0);
    int contextAfter = ParseIntField(input, "context_after", 0);
    if (headLimit <= 0) headLimit = kDefaultHeadLimit;

    fs::path target(searchPath);
    if (!fs::exists(target)) {
        return "Error: Path not found: " + searchPath;
    }

    std::regex::flag_type flags = std::regex::ECMAScript;
    if (caseInsensitive) flags |= std::regex::icase;

    std::regex re;
    try {
        re = std::regex(pattern, flags);
    } catch (const std::regex_error& e)
    {
        return "Error: invalid regex pattern: " + std::string(e.what());
    }

    std::vector<std::string> matchingFiles;
    std::vector<std::string> blocks;
    int resultChars = 0;
    bool truncated = false;
    int skippedBinary = 0;
    int skippedLarge = 0;
    std::map<std::string, int> counts;
    std::map<std::string, double> fileMtimes;

    try {
        if (fs::is_regular_file(target)) {
            // Single file search
            if (globFilter.empty() || GlobFilter(target.filename().string(), globFilter)) {
                ProcessFile(target, target.parent_path(), re, pattern, outputMode,
                           headLimit, contextBefore, contextAfter, matchingFiles, blocks,
                           counts, fileMtimes, resultChars, truncated, skippedBinary, skippedLarge);
            }
        } else if (fs::is_directory(target))
        {
            for (const auto& entry : fs::recursive_directory_iterator(target)) {
                bool skip = false;
                for (const auto& part : entry.path()) {
                    if (kIgnoreDirs.count(part.string())) {
                        skip = true; break;
                    }
                }
                if (skip) continue;
                if (!entry.is_regular_file()) continue;
                if (!GlobFilter(entry.path().filename().string(), globFilter)) continue;

                ProcessFile(entry.path(), target, re, pattern, outputMode,
                           headLimit, contextBefore, contextAfter, matchingFiles, blocks,
                           counts, fileMtimes, resultChars, truncated, skippedBinary, skippedLarge);
                if (truncated) break;
            }
        }
    } catch (const std::exception& e)
    {
        return "Error searching files: " + std::string(e.what());
    }

    std::ostringstream oss;
    if (outputMode == "files_with_matches") {
        if (matchingFiles.empty()) {
            return "No matches found for pattern '" + pattern + "' in " + searchPath;
        }
        for (size_t i = 0; i < matchingFiles.size() && static_cast<int>(i) < headLimit; ++i) {
            if (i > 0) oss << "\n";
            oss << matchingFiles[i];
        }
    } else if (outputMode == "count")
    {
        if (counts.empty()) {
            return "No matches found for pattern '" + pattern + "' in " + searchPath;
        }
        int shown = 0;
        for (const auto& p : counts) {
            if (shown >= headLimit) break;
            if (shown > 0) oss << "\n";
            oss << p.first << ": " << p.second;
            shown++;
        }
    } else {
        if (blocks.empty()) {
            return "No matches found for pattern '" + pattern + "' in " + searchPath;
        }
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (i > 0) oss << "\n\n";
            oss << blocks[i];
        }
    }

    std::vector<std::string> notes;
    if (truncated) notes.push_back("(output truncated)");
    if (skippedBinary > 0) notes.push_back("(skipped " + std::to_string(skippedBinary) + " binary files)");
    if (skippedLarge > 0) notes.push_back("(skipped " + std::to_string(skippedLarge) + " large files)");
    if (!notes.empty()) {
        oss << "\n\n";
        for (size_t i = 0; i < notes.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << notes[i];
        }
    }

    return oss.str();
}

void GrepTool::ProcessFile(const fs::path& filePath, const fs::path& root,
                          const std::regex& re, const std::string& /*pattern*/,
                          const std::string& outputMode, int headLimit,
                          int contextBefore, int contextAfter,
                          std::vector<std::string>& matchingFiles,
                          std::vector<std::string>& blocks,
                          std::map<std::string, int>& counts,
                          std::map<std::string, double>& fileMtimes,
                          int& resultChars, bool& truncated,
                          int& skippedBinary, int& skippedLarge) {
    auto fsize = fs::file_size(filePath);
    if (fsize > static_cast<uintmax_t>(kMaxFileBytes)) { skippedLarge++; return; }

    std::ifstream file(filePath.string());
    if (!file.is_open()) { skippedBinary++; return; }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    if (IsBinary(content)) { skippedBinary++; return; }
    if (content.empty()) return;

    double mtime = 0.0;
    try { mtime = fs::last_write_time(filePath).time_since_epoch().count() / 1e9; } catch (...) {}

    std::string displayPath = fs::relative(filePath, root).generic_string();
    std::istringstream ss(content);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(ss, line)) lines.push_back(line);

    bool fileHadMatch = false;
    for (size_t idx = 0; idx < lines.size(); ++idx) {
        if (!std::regex_search(lines[idx], re)) continue;
        fileHadMatch = true;

        if (outputMode == "count") {
            counts[displayPath]++;
            continue;
        }
        if (outputMode == "files_with_matches") {
            if (std::find(matchingFiles.begin(), matchingFiles.end(), displayPath) == matchingFiles.end()) {
                matchingFiles.push_back(displayPath);
                fileMtimes[displayPath] = mtime;
            }
            break;
        }

        // Content mode
        if (static_cast<int>(blocks.size()) >= headLimit) { truncated = true; break; }

        int startLine = static_cast<int>(std::max(static_cast<int>(idx) - contextBefore, 0));
        int endLine = static_cast<int>(std::min(idx + static_cast<size_t>(contextAfter) + 1, lines.size()));

        std::ostringstream block;
        block << displayPath << ":" << (idx + 1);
        for (int l = startLine; l < endLine; ++l) {
            block << "\n";
            if (static_cast<size_t>(l) == idx) block << "> ";
            else block << "  ";
            block << (l + 1) << "| " << lines[l];
        }
        std::string blockStr = block.str();

        if (resultChars + static_cast<int>(blockStr.length()) > kMaxResultChars) {
            truncated = true;
            break;
        }
        blocks.push_back(blockStr);
        resultChars += static_cast<int>(blockStr.length());
    }

    if ((outputMode == "count" || outputMode == "files_with_matches") && fileHadMatch) {
        if (std::find(matchingFiles.begin(), matchingFiles.end(), displayPath) == matchingFiles.end()) {
            matchingFiles.push_back(displayPath);
            fileMtimes[displayPath] = mtime;
        }
    }
}

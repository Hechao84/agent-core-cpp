#include "file_state_tool.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <ctime>

namespace fs = std::filesystem;

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
        if (json[valEnd] == '\\' && valEnd + 1 < json.length()) { valEnd += 2; continue; }
        if (json[valEnd] == '"') break;
        valEnd++;
    }
    return json.substr(valStart + 1, valEnd - valStart - 1);
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

FileStateTool::FileStateTool()
    : Tool("file_state", "Manage file read/write state tracking for agent memory. "
         "Actions: check (check if file was read before editing), "
         "record_read (mark file as read), record_write (mark file as written), "
         "clear (clear all state). "
         "Input: JSON with 'action' (required), 'path' (for check/record_read/record_write).",
          {{"action", "Action: check, record_read, record_write, or clear", "string", true},
           {"path", "File path to track", "string", false}}) {}

void FileStateTool::SetStateFile(const std::string& path) { stateFile_ = path; }

std::string FileStateTool::Invoke(const std::string& input) {
    std::string action = ParseStringField(input, "action");
    std::string filePath = ParseStringField(input, "path");
    int offset = ParseIntField(input, "offset", 1);
    int limit = ParseIntField(input, "limit", 2000);

    LoadState();

    if (action == "check") {
        if (filePath.empty()) return "Error: 'path' parameter is required for check";
        fs::path fp(filePath);
        if (!fs::exists(fp)) return "Error: File not found: " + filePath;

        auto it = stateMap_.find(fp.string());
        if (it == stateMap_.end()) {
            return "[WARNING] File " + filePath + " has not been read. Please read it first before editing.";
        }

        FileState& state = it->second;
        auto mtime = fs::last_write_time(fp);
        auto mtimeSec = std::chrono::duration_cast<std::chrono::seconds>(
            mtime.time_since_epoch()).count();

        if (mtimeSec != state.lastMtime) {
            return "[WARNING] File " + filePath + " has been modified since last read. Please read it again.";
        }
        if (offset != state.lastReadOffset || limit != state.lastReadLimit) {
            return "[WARNING] File " + filePath + " was read with different offset/limit. Please read with offset=" + std::to_string(state.lastReadOffset) + " and limit=" + std::to_string(state.lastReadLimit) + " first.";
        }
        return "[OK] File " + filePath + " state is valid for editing";
    }

    if (action == "record_read") {
        if (filePath.empty()) return "Error: 'path' parameter is required for record_read";
        fs::path fp(filePath);
        if (!fs::exists(fp)) return "Error: File not found: " + filePath;

        FileState& state = stateMap_[fp.string()];
        auto mtime = fs::last_write_time(fp);
        state.lastMtime = std::chrono::duration_cast<std::chrono::seconds>(
            mtime.time_since_epoch()).count();
        state.lastReadTime = std::time(nullptr);
        state.lastReadOffset = offset;
        state.lastReadLimit = limit;
        state.lastWriteTime = 0;
        SaveState();
        return "Marked " + filePath + " as read (offset=" + std::to_string(offset) +
               ", limit=" + std::to_string(limit) + ")";
    }

    if (action == "record_write") {
        if (filePath.empty()) return "Error: 'path' parameter is required for record_write";
        fs::path fp(filePath);
        auto it = stateMap_.find(fp.string());
        if (it != stateMap_.end()) {
            it->second.lastWriteTime = std::time(nullptr);
        } else {
            FileState state;
            state.lastWriteTime = std::time(nullptr);
            stateMap_[fp.string()] = state;
        }
        SaveState();
        return "Marked " + filePath + " as written";
    }

    if (action == "clear") {
        stateMap_.clear();
        SaveState();
        return "Cleared all file state";
    }

    return "Error: Unknown action '" + action + "'. Use: check, record_read, record_write, or clear";
}

void FileStateTool::LoadState() {
    stateMap_.clear();
    if (!fs::exists(stateFile_)) return;

    std::ifstream file(stateFile_);
    if (!file.is_open()) return;

    std::string line;
    FileState current;
    std::string currentPath;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        if (line.substr(0, 5) == "file:") {
            if (!currentPath.empty()) {
                stateMap_[currentPath] = current;
            }
            currentPath = line.substr(5);
            current = FileState();
        } else if (line.substr(0, 7) == "mtime: ") {
            current.lastMtime = std::stoll(line.substr(7));
        } else if (line.substr(0, 11) == "read_time: ") {
            current.lastReadTime = std::stoll(line.substr(11));
        } else if (line.substr(0, 12) == "write_time: ") {
            current.lastWriteTime = std::stoll(line.substr(12));
        } else if (line.substr(0, 8) == "offset: ") {
            current.lastReadOffset = std::stoi(line.substr(8));
        } else if (line.substr(0, 7) == "limit: ") {
            current.lastReadLimit = std::stoi(line.substr(7));
        }
    }
    if (!currentPath.empty()) {
        stateMap_[currentPath] = current;
    }
    file.close();
}

void FileStateTool::SaveState() {
    if (stateFile_.empty()) return;
    fs::path statePath(stateFile_);
    fs::path parentDir = statePath.parent_path();
    if (!parentDir.empty() && !fs::exists(parentDir)) {
        fs::create_directories(parentDir);
    }

    std::ofstream file(stateFile_, std::ios::out | std::ios::trunc);
    if (!file.is_open()) return;

    file << "# File state tracking for agent memory\n";
    for (const auto& entry : stateMap_) {
        file << "file:" << entry.first << "\n";
        file << "mtime: " << entry.second.lastMtime << "\n";
        file << "read_time: " << entry.second.lastReadTime << "\n";
        file << "write_time: " << entry.second.lastWriteTime << "\n";
        file << "offset: " << entry.second.lastReadOffset << "\n";
        file << "limit: " << entry.second.lastReadLimit << "\n\n";
    }
    file.close();
}

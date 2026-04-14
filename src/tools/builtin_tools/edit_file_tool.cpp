#include "edit_file_tool.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

static std::string ParseStringField(const std::string& json, const std::string& key)
{
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";
    size_t colonPos = json.find(':', keyPos + searchKey.length());
    if (colonPos == std::string::npos) return "";
    size_t valStart = json.find_first_not_of(" \t", colonPos + 1);
    if (valStart == std::string::npos) return "";
    if (json[valStart] == 't') return "true";
    if (json[valStart] == 'f') return "false";
    if (json[valStart] != '"') return "";
    std::string result;
    size_t i = valStart + 1;
    while (i < json.length()) {
        if (json[i] == '\\' && i + 1 < json.length()) {
            char next = json[i + 1];
            if (next == 'n') result += '\n';
            else if (next == 't') result += '\t';
            else if (next == '"') result += '"';
            else if (next == '\\') result += '\\';
            else result += next;
            i += 2;
            continue;
        }
        if (json[i] == '"') break;
        result += json[i];
        i++;
    }
    return result;
}

static bool ParseBoolField(const std::string& json, const std::string& key, bool defaultVal)
{
    std::string val = ParseStringField(json, key);
    if (val == "true") return true;
    if (val == "false") return false;
    return defaultVal;
}

EditFileTool::EditFileTool()
    : Tool("edit_file", "Edit a file by replacing old_text with new_text. "
         "Tolerates minor whitespace differences. "
         "Input: JSON with 'path', 'old_text' (required), "
         "'new_text' (required), 'replace_all' (optional, default false).",
         {{"path", "The file path to edit", "string", true},
          {"old_text", "The text to find and replace", "string", true},
          {"new_text", "The new text to replace with", "string", true},
          {"replace_all", "Replace all occurrences (default false)", "boolean", false}}) {}

std::string EditFileTool::Invoke(const std::string& input)
{
    std::string filePath = ParseStringField(input, "path");
    if (filePath.empty()) {
        return "Error: 'path' parameter is required";
    }
    std::string oldText = ParseStringField(input, "old_text");
    std::string newText = ParseStringField(input, "new_text");
    bool replaceAll = ParseBoolField(input, "replace_all", false);

    if (oldText.empty() && newText.empty()) {
        return "Error: 'old_text' and/or 'new_text' parameter is required";
    }
    if (oldText.empty() && !newText.empty()) {
        // Create new file mode
        fs::path fp(filePath);
        if (fs::exists(fp)) {
            // Check if file is empty
            if (fs::file_size(fp) > 0) {
                return "Error: Cannot create file — " + filePath +
                       " already exists and is not empty.";
            }
        }
        fs::create_directories(fp.parent_path());
        std::ofstream file(filePath, std::ios::out | std::ios::trunc);
        if (!file.is_open()) {
            return "Error: Cannot create file: " + filePath;
        }
        file << newText;
        file.close();
        return "Successfully created " + filePath;
    }

    fs::path fp(filePath);
    if (!fs::exists(fp)) {
        std::string msg = "Error: File not found: " + filePath;
        // Suggest similar files
        fs::path parentDir = fp.parent_path();
        if (fs::exists(parentDir) && fs::is_directory(parentDir)) {
            std::vector<std::string> suggestions;
            for (const auto& entry : fs::directory_iterator(parentDir)) {
                if (entry.is_regular_file()) {
                    suggestions.push_back(entry.path().filename().string());
                }
            }
            if (!suggestions.empty()) {
                msg += "\nDid you mean: ";
                for (size_t i = 0; i < std::min(suggestions.size(), size_t(3)); ++i) {
                    if (i > 0) msg += ", ";
                    msg += suggestions[i];
                }
                msg += "?";
            }
        }
        return msg;
    }

    std::ifstream file(filePath);
    if (!file.is_open()) {
        return "Error: Cannot open file for editing: " + filePath;
    }
    std::ostringstream contentSs;
    contentSs << file.rdbuf();
    file.close();
    std::string content = contentSs.str();

    // Check for .ipynb files
    if (fp.extension() == ".ipynb") {
        return "Error: This is a Jupyter notebook. Use the notebook_edit tool instead.";
    }

    size_t firstMatch = content.find(oldText);
    if (firstMatch == std::string::npos) {
        // Try case-insensitive/trimmed match (simplified in C++)
        return "Error: old_text not found in " + filePath +
               ". Copy the exact text from read_file and try again.";
    }

    size_t count = 1;
    if (replaceAll) {
        size_t pos = firstMatch + oldText.length();
        while ((pos = content.find(oldText, pos)) != std::string::npos) {
            count++;
            pos += oldText.length();
        }
    }

    if (count > 1 && !replaceAll) {
        // Find all match positions
        std::vector<size_t> positions;
        size_t pos = firstMatch;
        while (pos != std::string::npos) {
            positions.push_back(pos);
            pos = content.find(oldText, pos + oldText.length());
        }
        int lineNum = 1;
        for (size_t i = 0; i < firstMatch && i < content.length(); ++i) {
            if (content[i] == '\n') lineNum++;
        }
        return "Warning: old_text appears " + std::to_string(count) +
               " times at line " + std::to_string(lineNum) +
               ". Provide more context to make it unique, or set replace_all=true.";
    }

    // Perform replacement
    std::string newContent;
    if (replaceAll) {
        newContent = content;
        size_t pos = 0;
        while ((pos = newContent.find(oldText, pos)) != std::string::npos) {
            newContent.replace(pos, oldText.length(), newText);
            pos += newText.length();
        }
    } else {
        newContent = content;
        newContent.replace(firstMatch, oldText.length(), newText);
    }

    std::ofstream outFile(filePath, std::ios::out | std::ios::trunc);
    if (!outFile.is_open()) {
        return "Error: Cannot write to file: " + filePath;
    }
    outFile << newContent;
    outFile.close();

    return "Successfully edited " + filePath;
}

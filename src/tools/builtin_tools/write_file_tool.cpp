

#include "src/tools/builtin_tools/write_file_tool.h"
#include <fstream>
#include <string>
#include "filesystem"

namespace fs = std::filesystem;static std::string ParseStringField(const std::string& json, const std::string& key)
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
            valEnd += 2; continue;
        }
        if (json[valEnd] == '"') break;
        valEnd++;
    }
    return json.substr(valStart + 1, valEnd - valStart - 1);
}

WriteFileTool::WriteFileTool()
    : Tool("write_file", "Write content to a file. Overwrites existing files; "
         "creates parent directories as needed. "
         "Input: JSON with 'path' and 'content' (both required).",
         {{"path", "The file path to write to", "string", true},
          {"content", "The content to write", "string", true}}){} std::string WriteFileTool::Invoke(const std::string& input)
{
    std::string filePath = ParseStringField(input, "path");
    if (filePath.empty()) {
        return "Error: 'path' parameter is required";
    }
    size_t contentKeyPos = input.find("\"content\"");
    if (contentKeyPos == std::string::npos) {
        return "Error: 'content' parameter is required";
    }

    // Extract content: find the colon after "content", then find the string value
    size_t colonPos = input.find(':', contentKeyPos + 9);
    if (colonPos == std::string::npos) {
        return "Error: invalid input format for 'content'";
    }
    size_t valStart = input.find_first_not_of(" \t", colonPos + 1);
    if (valStart == std::string::npos) {
        return "Error: 'content' parameter is required";
    }
    std::string content;
    if (input[valStart] == '"') {
        // String value - parse the JSON string
        size_t valEnd = valStart + 1;
        while (valEnd < input.length()) {
            if (input[valEnd] == '\\' && valEnd + 1 < input.length()) {
                content += input[valEnd + 1];
                valEnd += 2;
                continue;
            }
            if (input[valEnd] == '"') break;
            content += input[valEnd];
            valEnd++;
        }
    } else {
        content = input.substr(valStart);
    }

    try {
        fs::path fp(filePath);
        fs::create_directories(fp.parent_path());
        std::ofstream file(filePath, std::ios::out | std::ios::trunc);
        if (!file.is_open()) {
            return "Error: Cannot open file for writing: " + filePath;
        }
        file << content;
        file.close();
        return "Successfully wrote " + std::to_string(content.length()) +
               " characters to " + filePath;
    } catch (const std::exception& e) {
        return "Error writing file: " + std::string(e.what());
    }
}

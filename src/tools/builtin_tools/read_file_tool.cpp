#include "src/tools/builtin_tools/read_file_tool.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace jiuwen {

static const int kMaxChars = 128000;
static const int kDefaultLimit = 2000;static std::string ParseStringField(const std::string& json, const std::string& key)
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
    std::string result;
    while (valEnd < json.length()) {
        if (json[valEnd] == '\\' && valEnd + 1 < json.length()) {
            valEnd += 2;
            continue;
        }
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
    try {
        return std::stoi(json.substr(valStart));
    } catch (...) {
        return defaultVal;
    }
}

ReadFileTool::ReadFileTool()
    : Tool("read_file", "Read file contents with pagination. "
         "Input: JSON with 'path' (required), 'offset' (1-based line, default 1), "
         "'limit' (max lines, default 2000). "
         "Output format: LINE_NUM|CONTENT. Truncated at ~128K chars.",
         {{"path", "The file path to read", "string", true},
          {"offset", "1-based line number to start from", "integer", false},
          {"limit", "Maximum lines to read (default 2000)", "integer", false}}){} std::string ReadFileTool::Invoke(const std::string& input)
{
    std::string filePath = ParseStringField(input, "path");
    if (filePath.empty()) {
        return "Error: 'path' parameter is required";
    }

    int offset = ParseIntField(input, "offset", 1);
    int limit = ParseIntField(input, "limit", kDefaultLimit);

    if (offset < 1) offset = 1;
    if (limit < 1) limit = kDefaultLimit;

    fs::path fp(filePath);
    if (!fs::exists(fp)) {
        return "Error: File not found: " + filePath;
    }
    if (!fs::is_regular_file(fp)) {
        return "Error: Not a file: " + filePath;
    }

    std::ifstream file(filePath);
    if (!file.is_open()) {
        return "Error: Cannot open file: " + filePath;
    }

    std::string line;
    std::vector<std::string> allLines;
    while (std::getline(file, line)) {
        allLines.push_back(line);
    }
    file.close();

    int total = static_cast<int>(allLines.size());
    if (total == 0) {
        return "(Empty file: " + filePath + ")";
    }

    if (offset > total) {
        return "Error: offset " + std::to_string(offset) +
               " is beyond end of file (" + std::to_string(total) + " lines)";
    }

    int start = offset - 1;
    int end = std::min(start + limit, total);

    std::ostringstream oss;
    int charCount = 0;
    int actualEnd = start;
    for (int i = start; i < end; ++i) {
        std::string lineEntry = std::to_string(i + 1) + "| " + allLines[i];
        if (charCount + static_cast<int>(lineEntry.length()) + 1 > kMaxChars) {
            break;
        }
        if (i > start) oss << "\n";
        oss << lineEntry;
        charCount += static_cast<int>(lineEntry.length()) + 1;
        actualEnd = i + 1;
    }

    if (actualEnd < total) {
        oss << "\n\n(Showing lines " << offset << "-" << actualEnd << " of " << total
            << ". Use offset=" << (actualEnd + 1) << " to continue.)";
    } else {
        oss << "\n\n(End of file — " << total << " lines total)";
    }

    return oss.str();
}

} // namespace jiuwen

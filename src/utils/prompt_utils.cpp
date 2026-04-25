#include "src/utils/prompt_utils.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace jiuwen {

std::string ReadFileContent(const std::string& filePath)
{
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "Warning: Failed to open prompt resource file: " << filePath << std::endl;
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string ResolvePromptResource(const PromptResource& resource)
{
    if (resource.type == PromptResourceType::FILE_PATH) {
        return ReadFileContent(resource.value);
    }
    return resource.value;
}

std::string RenderPrompt(const std::string& templateStr, const std::unordered_map<std::string, std::string>& vars)
{
    std::string result = templateStr;
    std::string::size_type pos = 0;

    while ((pos = result.find("{$", pos)) != std::string::npos) {
        std::string::size_type endPos = result.find("}", pos);
        if (endPos == std::string::npos) break;

        std::string key = result.substr(pos + 2, endPos - pos - 2);
        std::string replacement = "";
        auto it = vars.find(key);
        if (it != vars.end()) {
            replacement = it->second;
        }

        result.replace(pos, endPos - pos + 1, replacement);
        
        // Update pos to after the replacement to continue searching
        pos += replacement.length();
    }

    return result;
}

} // namespace jiuwen

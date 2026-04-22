#pragma once

#include <string>
#include <unordered_map>
#include "include/types.h"

namespace PromptUtils {

// ResolvePromptResource: Resolves a PromptResource to its actual string content.
// If type is FILE_PATH, reads the file. If type is TEXT, returns the value as-is.
std::string ResolvePromptResource(const PromptResource& resource);

// RenderPrompt: Replaces placeholders like {$KEY} in the template with values from the map.
std::string RenderPrompt(const std::string& templateStr, const std::unordered_map<std::string, std::string>& vars);

} // namespace PromptUtils

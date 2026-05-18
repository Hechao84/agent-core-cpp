#pragma once

#include <string>
#include <vector>

namespace jiuwen {

struct ToolCall
{
    std::string name;
    std::string arguments;
};

std::string ExtractJson(const std::string& text, size_t startPos);
std::vector<ToolCall> ExtractAllToolCalls(const std::string& response);
std::string ParseAction(const std::string& response, std::string& actionInput);
std::string TrimStr(const std::string& str);

} // namespace jiuwen

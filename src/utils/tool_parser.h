#pragma once

#include <string>
#include <vector>

namespace jiuwen {

// Parsed tool-call descriptor used by the prompt-only (fallback) ReAct path.
// Distinct from include/model.h's structured ToolCall, which is the
// canonical wire-level representation produced by native function-calling.
struct ParsedToolCall
{
    std::string name;
    std::string arguments;
};

std::string ExtractJson(const std::string& text, size_t startPos);
std::vector<ParsedToolCall> ExtractAllToolCalls(const std::string& response);
std::string ParseAction(const std::string& response, std::string& actionInput);
std::string TrimStr(const std::string& str);

} // namespace jiuwen

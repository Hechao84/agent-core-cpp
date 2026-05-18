#include "examples/jiuwenClaw/utils/string_utils.h"

namespace jiuwenClaw {

std::string TrimStr(const std::string& str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

} // namespace jiuwenClaw

#include "agent_response_handler.h"

namespace jiuwenClaw {

namespace {

static std::string TrimStr(const std::string& str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

} // namespace

AgentResponseHandler::AgentResponseHandler(bool enableStreamCarry)
    : enableStreamCarry_(enableStreamCarry)
{
}

AgentResponseHandler::OutputCallback AgentResponseHandler::GetCallback() const
{
    return [this](const std::string& resp) {
        HandleResponse(resp);
    };
}

std::string AgentResponseHandler::GetFullResponse() const
{
    return fullResponse_;
}

void AgentResponseHandler::Reset()
{
    utf8Carry_.clear();
    fullResponse_.clear();
}

void AgentResponseHandler::HandleResponse(const std::string& resp) const
{
    if (resp.empty()) return;

    std::string s = resp;

    if (s.find("[TOOL_CALLS]") != std::string::npos) return;

    if (s.find("[TOOL_RESPONSE]") != std::string::npos) {
        std::string content = ExtractTagContent(s, "[TOOL_RESPONSE]");
        if (!content.empty()) {
            std::cout << "\n\n**Tool Response**: \n" << UTF8ToLocal(content) << std::endl;
        }
        return;
    }

    if (s.find("[STREAM]") != std::string::npos) {
        std::string content = ExtractTagContent(s, "[STREAM]");
        if (!content.empty()) {
            if (enableStreamCarry_) {
                std::string fixed = FixUTF8Streaming(content, utf8Carry_);
                std::cout << UTF8ToLocal(fixed) << std::flush;
            } else {
                std::cout << UTF8ToLocal(content) << std::flush;
            }
        }
        return;
    }

    if (s.find("[STATUS]") != std::string::npos) {
        std::string content = ExtractTagContent(s, "[STATUS]");
        std::string formatted = "\n>> " + TrimStr(content) + "\n";
        std::cout << UTF8ToLocal(formatted) << std::flush;
        return;
    }

    if (s.find("[FINAL]") != std::string::npos) return;

    if (!s.empty()) {
        std::cout << UTF8ToLocal(s) << std::flush;
    }
}

std::string AgentResponseHandler::ExtractTagContent(const std::string& s, const std::string& tag) const
{
    size_t start_pos = s.find(tag);
    if (start_pos == std::string::npos) return "";
    start_pos += tag.length();
    while (start_pos < s.length() && s[start_pos] == ' ') {
        ++start_pos;
    }
    return s.substr(start_pos);
}

} // namespace jiuwenClaw

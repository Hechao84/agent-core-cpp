#include "src/context_engine/storage_base.h"

namespace jiuwen {

ContextStorageBase::ContextStorageBase(const std::string& sessionId)
    : sessionId_(sessionId)
{
}

bool ContextStorageBase::IsValidMessage(const Message& msg)
{
    return !msg.content.empty();
}

std::string ContextStorageBase::CleanMessageContent(const std::string& input)
{
    std::string output;
    output.reserve(input.length());

    bool inEscape = false;

    for (size_t i = 0; i < input.length(); ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);

        // Start of ANSI escape sequence: ESC (0x1B) followed by [
        if (!inEscape && c == 0x1B && i + 1 < input.length() && input[i + 1] == '[') {
            inEscape = true;
            continue;
        }

        // Inside escape sequence: skip until terminator (A-Z or a-z)
        if (inEscape) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == 'm') {
                inEscape = false;
            }
            continue;
        }

        // Skip control characters except \t, \n, \r
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
            continue;
        }

        // Skip DEL (0x7F, backspace on some terminals)
        if (c == 0x7F) {
            continue;
        }

        output += input[i];
    }

    return output;
}

} // namespace jiuwen

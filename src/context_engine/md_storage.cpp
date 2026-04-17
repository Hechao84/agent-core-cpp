

#include "src/context_engine/md_storage.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "filesystem"

namespace fs = std::filesystem;

// Remove ANSI escape sequences and terminal control characters.
// This cleans up artifacts from user editing (backspace, arrows, etc.)
static std::string CleanControlChars(const std::string& input)
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

MarkdownStorage::MarkdownStorage(const std::string& path, const std::string& sessionId)
{
    fs::path dir(path);
    if (!fs::exists(dir)) {
        fs::create_directories(dir);
    }
    filePath_ = (dir / (sessionId + ".md")).string();
}

bool MarkdownStorage::SaveMessage(const Message& msg)
{
    // Skip messages with empty content to avoid noise in session file
    if (msg.content.empty()) return true;
    try {
        std::ofstream file(filePath_, std::ios::app);
        if (!file.is_open()) return false;
        file << FormatMessage(msg);
        return true;
    } catch (...) {
        return false;
    }
}

bool MarkdownStorage::LoadHistory(std::vector<Message>& outMessages)
{
    if (!fs::exists(filePath_)) return true; // Empty is okay
    try {
        std::ifstream file(filePath_);
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        size_t pos = 0;
        while ((pos = content.find("## ", pos)) != std::string::npos) {
            size_t nextPos = content.find("## ", pos + 3);
            std::string block = content.substr(pos, nextPos == std::string::npos ? std::string::npos : nextPos - pos);
            Message msg = ParseMessageBlock(block);
            if (!msg.role.empty() && !msg.content.empty()) {
                outMessages.push_back(msg);
            }
            if (nextPos == std::string::npos) break;
            pos = nextPos;
        }
    } catch (...) {
        return false;
    }
    return true;
}

void MarkdownStorage::Clear()
{
    try {
        if (fs::exists(filePath_)) {
            fs::remove(filePath_);
        }
    } catch (...){} }

std::string MarkdownStorage::FormatMessage(const Message& msg) const
{
    return "\n## " + msg.role + "\n\n" + CleanControlChars(msg.content) + "\n";
}

Message MarkdownStorage::ParseMessageBlock(const std::string& block) const
{
    Message msg;
    size_t start = block.find("## ");
    if (start == std::string::npos) return msg;
    size_t nl = block.find('\n', start);
    if (nl == std::string::npos) return msg;

    msg.role = block.substr(start + 3, nl - start - 3);
    size_t contentStart = block.find_first_not_of("\r\n ", nl);
    if (contentStart != std::string::npos) {
        // Check if there's actual content before the next "##"
        size_t nextHeader = block.find("\n## ", contentStart);
        std::string rawContent;
        if (nextHeader != std::string::npos) {
            rawContent = block.substr(contentStart, nextHeader - contentStart);
        } else {
            rawContent = block.substr(contentStart);
        }
        // Trim the content
        size_t first = rawContent.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            msg.content = rawContent.substr(first);
            size_t last = msg.content.find_last_not_of(" \t\r\n");
            if (last != std::string::npos) {
                msg.content = msg.content.substr(0, last + 1);
            }
        }
    }
    return msg;
}

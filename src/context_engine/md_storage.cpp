#include "src/context_engine/md_storage.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "filesystem"

namespace fs = std::filesystem;

namespace jiuwen {

MarkdownStorage::MarkdownStorage(const std::string& path, const std::string& sessionId)
    : ContextStorageBase(sessionId)
{
    fs::path dir(path);
    if (!fs::exists(dir)) {
        fs::create_directories(dir);
    }
    filePath_ = (dir / (sessionId_ + ".md")).string();
}

bool MarkdownStorage::SaveMessage(const Message& msg)
{
    if (!IsValidMessage(msg)) return true;
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
    // Ensure role is clean (no \r or trailing whitespace) before writing
    std::string cleanRole = msg.role;
    size_t r1 = cleanRole.find_first_not_of(" \t\r\n");
    if (r1 != std::string::npos) cleanRole.erase(0, r1);
    size_t r2 = cleanRole.find_last_not_of(" \t\r\n");
    if (r2 != std::string::npos) cleanRole.erase(r2 + 1);
    return "\n## " + cleanRole + "\n\n" + CleanMessageContent(msg.content) + "\n";
}

Message MarkdownStorage::ParseMessageBlock(const std::string& block) const
{
    Message msg;
    size_t start = block.find("## ");
    if (start == std::string::npos) return msg;
    size_t nl = block.find('\n', start);
    if (nl == std::string::npos) return msg;

    msg.role = block.substr(start + 3, nl - start - 3);
    // Trim \r and whitespace from role (handles Windows \r\n line endings)
    size_t r1 = msg.role.find_first_not_of(" \t\r\n");
    if (r1 != std::string::npos) msg.role.erase(0, r1);
    size_t r2 = msg.role.find_last_not_of(" \t\r\n");
    if (r2 != std::string::npos) msg.role.erase(r2 + 1);
    if (msg.role.empty()) return msg;
    size_t contentStart = block.find_first_not_of("\r\n ", nl);
    if (contentStart != std::string::npos) {
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

} // namespace jiuwen

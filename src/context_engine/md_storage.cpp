#include "md_storage.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

MarkdownStorage::MarkdownStorage(const std::string& path, const std::string& sessionId) {
    fs::path dir(path);
    if (!fs::exists(dir)) {
        fs::create_directories(dir);
    }
    m_filePath = (dir / (sessionId + ".md")).string();
}

bool MarkdownStorage::SaveMessage(const Message& msg) {
    try {
        std::ofstream file(m_filePath, std::ios::app);
        if (!file.is_open()) return false;
        file << FormatMessage(msg);
        return true;
    } catch (...) {
        return false;
    }
}

bool MarkdownStorage::LoadHistory(std::vector<Message>& outMessages) {
    if (!fs::exists(m_filePath)) return true; // Empty is okay
    try {
        std::ifstream file(m_filePath);
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        size_t pos = 0;
        while ((pos = content.find("## ", pos)) != std::string::npos) {
            size_t nextPos = content.find("## ", pos + 3);
            std::string block = content.substr(pos, nextPos == std::string::npos ? std::string::npos : nextPos - pos);
            Message msg = ParseMessageBlock(block);
            if (!msg.role.empty()) {
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

void MarkdownStorage::Clear() {
    try {
        if (fs::exists(m_filePath)) {
            fs::remove(m_filePath);
        }
    } catch (...) {}
}

std::string MarkdownStorage::FormatMessage(const Message& msg) const {
    return "\n## " + msg.role + "\n\n" + msg.content + "\n";
}

Message MarkdownStorage::ParseMessageBlock(const std::string& block) const {
    Message msg;
    size_t start = block.find("## ");
    if (start == std::string::npos) return msg;
    size_t nl = block.find('\n', start);
    if (nl == std::string::npos) return msg;
    
    msg.role = block.substr(start + 3, nl - start - 3);
    size_t contentStart = block.find_first_not_of("\r\n ", nl);
    if (contentStart != std::string::npos) {
        msg.content = block.substr(contentStart);
    }
    return msg;
}

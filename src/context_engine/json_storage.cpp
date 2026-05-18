#include "src/context_engine/json_storage.h"
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "filesystem"
#include "third_party/include/nlohmann/json.hpp"

namespace fs = std::filesystem;

namespace jiuwen {

JsonStorage::JsonStorage(const std::string& path, const std::string& sessionId)
    : ContextStorageBase(sessionId)
{
    fs::path dir(path);
    if (!fs::exists(dir)) {
        fs::create_directories(dir);
    }
    filePath_ = (dir / "history.json").string();
}

bool JsonStorage::SaveMessage(const Message& msg)
{
    if (!IsValidMessage(msg)) return true;
    try {
        // Read existing history
        nlohmann::json history = nlohmann::json::array();
        if (fs::exists(filePath_)) {
            std::ifstream inFile(filePath_);
            if (inFile.is_open()) {
                history = nlohmann::json::parse(inFile, nullptr, false);
                if (history.is_discarded() || !history.is_array()) {
                    history = nlohmann::json::array();
                }
            }
        }

        // Append new message
        nlohmann::json entry;
        entry["role"] = msg.role;
        entry["content"] = msg.content;
        history.push_back(entry);

        // Write atomically via temp file
        std::string tmpPath = filePath_ + ".tmp";
        std::ofstream outFile(tmpPath, std::ios::trunc);
        if (!outFile.is_open()) return false;
        outFile << history.dump(2);
        outFile.close();

        fs::rename(tmpPath, filePath_);
        return true;
    } catch (...) {
        return false;
    }
}

bool JsonStorage::LoadHistory(std::vector<Message>& outMessages)
{
    if (!fs::exists(filePath_)) return true;
    try {
        std::ifstream inFile(filePath_);
        if (!inFile.is_open()) return false;

        nlohmann::json history = nlohmann::json::parse(inFile, nullptr, false);
        if (history.is_discarded() || !history.is_array()) return true;

        for (const auto& entry : history) {
            if (entry.contains("role") && entry.contains("content")) {
                Message msg;
                msg.role = entry["role"].get<std::string>();
                msg.content = entry["content"].get<std::string>();
                if (!msg.role.empty() && !msg.content.empty()) {
                    outMessages.push_back(msg);
                }
            }
        }
    } catch (...) {
        return false;
    }
    return true;
}

void JsonStorage::Clear()
{
    try {
        if (fs::exists(filePath_)) {
            fs::remove(filePath_);
        }
    } catch (...) {}
}

} // namespace jiuwen

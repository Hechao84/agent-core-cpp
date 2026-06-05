#include "src/context_engine/json_storage.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "third_party/include/nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

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

namespace {

json EncodeMessage(const Message& msg)
{
    json entry;
    entry["role"] = msg.role;
    entry["content"] = msg.content;
    if (!msg.toolCalls.empty()) {
        json arr = json::array();
        for (const auto& tc : msg.toolCalls) {
            json j;
            j["id"] = tc.id;
            j["name"] = tc.name;
            j["arguments"] = tc.argumentsJson;
            arr.push_back(j);
        }
        entry["tool_calls"] = arr;
    }
    if (!msg.toolCallId.empty()) {
        entry["tool_call_id"] = msg.toolCallId;
    }
    if (!msg.toolName.empty()) {
        entry["tool_name"] = msg.toolName;
    }
    return entry;
}

bool DecodeMessage(const json& entry, Message& out)
{
    if (!entry.contains("role") || !entry["role"].is_string()) return false;
    out.role = entry["role"].get<std::string>();
    if (entry.contains("content") && entry["content"].is_string()) {
        out.content = entry["content"].get<std::string>();
    }
    if (entry.contains("tool_calls") && entry["tool_calls"].is_array()) {
        for (const auto& tc : entry["tool_calls"]) {
            ToolCall t;
            t.id = tc.value("id", "");
            t.name = tc.value("name", "");
            t.argumentsJson = tc.value("arguments", "");
            out.toolCalls.push_back(std::move(t));
        }
    }
    if (entry.contains("tool_call_id") && entry["tool_call_id"].is_string()) {
        out.toolCallId = entry["tool_call_id"].get<std::string>();
    }
    if (entry.contains("tool_name") && entry["tool_name"].is_string()) {
        out.toolName = entry["tool_name"].get<std::string>();
    }
    // Discard rows that have no usable payload.
    if (out.role.empty()) return false;
    if (out.role == "tool" && out.toolCallId.empty()) return false;
    if (out.role != "assistant" && out.content.empty()) return false;
    if (out.role == "assistant" && out.content.empty() && out.toolCalls.empty()) return false;
    return true;
}

} // namespace

bool JsonStorage::SaveMessage(const Message& msg)
{
    if (!IsValidMessage(msg)) return true;
    try {
        json history = json::array();
        if (fs::exists(filePath_)) {
            std::ifstream inFile(filePath_);
            if (inFile.is_open()) {
                history = json::parse(inFile, nullptr, false);
                if (history.is_discarded() || !history.is_array()) {
                    history = json::array();
                }
            }
        }
        history.push_back(EncodeMessage(msg));
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
        json history = json::parse(inFile, nullptr, false);
        if (history.is_discarded() || !history.is_array()) return true;
        for (const auto& entry : history) {
            Message msg;
            if (DecodeMessage(entry, msg)) {
                outMessages.push_back(std::move(msg));
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
    } catch (...) {
    }
}

} // namespace jiuwen

#include "src/context_engine/db_storage.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "third_party/include/nlohmann/json.hpp"
#include "third_party/include/sqlite3.h"

using json = nlohmann::json;

namespace jiuwen {

DbStorage::DbStorage(const std::string& dbPath, const std::string& sessionId)
    : ContextStorageBase(sessionId)
{
    if (sqlite3_open(dbPath.c_str(), &db_) != SQLITE_OK) {
        LogError("Failed to open database");
        db_ = nullptr;
    } else {
        InitDatabase();
    }
}

DbStorage::~DbStorage()
{
    if (db_) sqlite3_close(db_);
}

bool DbStorage::InitDatabase()
{
    if (!db_) return false;
    return CreateTable();
}

bool DbStorage::CreateTable()
{
    // Drop any pre-existing table whose schema is incompatible with v2.
    // We probe by selecting the new columns; if the SELECT fails the table
    // is either missing (CREATE will succeed) or stale (DROP + CREATE).
    bool needRecreate = false;
    {
        sqlite3_stmt* stmt = nullptr;
        const char* probe = "SELECT tool_calls, tool_call_id, tool_name, payload_ref FROM messages LIMIT 1;";
        if (sqlite3_prepare_v2(db_, probe, -1, &stmt, nullptr) != SQLITE_OK) {
            // Table exists with stale schema, or does not exist. Either way,
            // a DROP-if-exists + CREATE rebuilds it cleanly.
            std::string err = sqlite3_errmsg(db_);
            // Only treat "no such column" as a stale-schema signal.
            if (err.find("no such column") != std::string::npos) {
                needRecreate = true;
            }
        }
        if (stmt) sqlite3_finalize(stmt);
    }
    if (needRecreate) {
        char* errMsg = nullptr;
        if (sqlite3_exec(db_, "DROP TABLE IF EXISTS messages;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
            LogError("Failed to drop stale messages table");
            if (errMsg) sqlite3_free(errMsg);
            return false;
        }
        if (errMsg) sqlite3_free(errMsg);
    }

    const char* sql = "CREATE TABLE IF NOT EXISTS messages ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "session_id TEXT NOT NULL, "
                      "role TEXT NOT NULL, "
                      "content TEXT, "
                      "tool_calls TEXT, "        // JSON array, NULL when not assistant tool-calls
                      "tool_call_id TEXT, "
                      "tool_name TEXT, "
                      "payload_ref TEXT, "
                      "timestamp TEXT"
                      ");";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, 0, &errMsg);
    if (rc != SQLITE_OK) {
        LogError("Failed to create table");
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }

    // Migration: a v2-era DB (has the v2 columns from the probe above but
    // predates the timestamp column) passes the stale-schema probe and
    // CREATE TABLE IF NOT EXISTS is a no-op, so the timestamp column would
    // never be added -- SaveMessage/LoadHistory would then fail with
    // "no such column: timestamp". Probe for the column and ALTER if missing.
    // Existing rows keep NULL timestamp (their original event time was never
    // recorded); only newly added rows get an ISO 8601 timestamp via INSERT.
    {
        sqlite3_stmt* tsProbe = nullptr;
        const char* probe = "SELECT timestamp FROM messages LIMIT 1;";
        if (sqlite3_prepare_v2(db_, probe, -1, &tsProbe, nullptr) != SQLITE_OK) {
            std::string err = sqlite3_errmsg(db_);
            if (err.find("no such column") != std::string::npos) {
                char* alterErr = nullptr;
                if (sqlite3_exec(db_, "ALTER TABLE messages ADD COLUMN timestamp TEXT;",
                                 nullptr, nullptr, &alterErr) != SQLITE_OK) {
                    LogError("Failed to add timestamp column");
                    if (alterErr) sqlite3_free(alterErr);
                    return false;
                }
                if (alterErr) sqlite3_free(alterErr);
            }
        }
        if (tsProbe) sqlite3_finalize(tsProbe);
    }
    return true;
}

namespace {

std::string EncodeToolCalls(const std::vector<ToolCall>& calls)
{
    if (calls.empty()) return "";
    json arr = json::array();
    for (const auto& tc : calls) {
        json j;
        j["id"] = tc.id;
        j["name"] = tc.name;
        j["arguments"] = tc.argumentsJson;
        arr.push_back(j);
    }
    return arr.dump();
}

std::vector<ToolCall> DecodeToolCalls(const std::string& str)
{
    std::vector<ToolCall> out;
    if (str.empty()) return out;
    try {
        auto j = json::parse(str);
        if (!j.is_array()) return out;
        for (const auto& entry : j) {
            ToolCall tc;
            tc.id = entry.value("id", "");
            tc.name = entry.value("name", "");
            tc.argumentsJson = entry.value("arguments", "");
            out.push_back(std::move(tc));
        }
    } catch (...) {
    }
    return out;
}

} // namespace

bool DbStorage::SaveMessage(const Message& msg)
{
    if (!IsValidMessage(msg)) return true;
    if (!db_) return false;

    const char* sql = "INSERT INTO messages (session_id, role, content, tool_calls, tool_call_id, tool_name, payload_ref, timestamp) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LogError("Failed to prepare INSERT");
        return false;
    }

    std::string cleanContent = CleanMessageContent(msg.content);
    std::string toolCallsStr = EncodeToolCalls(msg.toolCalls);

    sqlite3_bind_text(stmt, 1, sessionId_.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, msg.role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, cleanContent.c_str(), -1, SQLITE_TRANSIENT);
    if (toolCallsStr.empty()) {
        sqlite3_bind_null(stmt, 4);
    } else {
        sqlite3_bind_text(stmt, 4, toolCallsStr.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (msg.toolCallId.empty()) {
        sqlite3_bind_null(stmt, 5);
    } else {
        sqlite3_bind_text(stmt, 5, msg.toolCallId.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (msg.toolName.empty()) {
        sqlite3_bind_null(stmt, 6);
    } else {
        sqlite3_bind_text(stmt, 6, msg.toolName.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (msg.payloadRef.empty()) {
        sqlite3_bind_null(stmt, 7);
    } else {
        sqlite3_bind_text(stmt, 7, msg.payloadRef.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (msg.timestamp.empty()) {
        sqlite3_bind_null(stmt, 8);
    } else {
        sqlite3_bind_text(stmt, 8, msg.timestamp.c_str(), -1, SQLITE_TRANSIENT);
    }

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    if (!ok) {
        LogError("Failed to save message");
    }
    return ok;
}

bool DbStorage::LoadHistory(std::vector<Message>& outMessages)
{
    if (!db_) return false;

    const char* sql = "SELECT role, content, tool_calls, tool_call_id, tool_name, payload_ref, timestamp "
                      "FROM messages WHERE session_id = ? ORDER BY id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LogError("Failed to load history");
        return false;
    }
    sqlite3_bind_text(stmt, 1, sessionId_.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Message msg;
        const unsigned char* role = sqlite3_column_text(stmt, 0);
        const unsigned char* content = sqlite3_column_text(stmt, 1);
        const unsigned char* toolCalls = sqlite3_column_text(stmt, 2);
        const unsigned char* toolCallId = sqlite3_column_text(stmt, 3);
        const unsigned char* toolName = sqlite3_column_text(stmt, 4);
        const unsigned char* payloadRef = sqlite3_column_text(stmt, 5);
        const unsigned char* timestamp = sqlite3_column_text(stmt, 6);
        if (role) msg.role = reinterpret_cast<const char*>(role);
        if (content) msg.content = reinterpret_cast<const char*>(content);
        if (toolCalls) msg.toolCalls = DecodeToolCalls(reinterpret_cast<const char*>(toolCalls));
        if (toolCallId) msg.toolCallId = reinterpret_cast<const char*>(toolCallId);
        if (toolName) msg.toolName = reinterpret_cast<const char*>(toolName);
        if (payloadRef) msg.payloadRef = reinterpret_cast<const char*>(payloadRef);
        if (timestamp) msg.timestamp = reinterpret_cast<const char*>(timestamp);
        if (msg.role.empty()) continue;
        if (msg.role == "tool" && msg.toolCallId.empty()) continue;
        if (msg.role == "assistant" && msg.content.empty() && msg.toolCalls.empty()) continue;
        outMessages.push_back(std::move(msg));
    }
    sqlite3_finalize(stmt);
    return true;
}

void DbStorage::Clear()
{
    if (!db_) return;
    const char* sql = "DELETE FROM messages WHERE session_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sessionId_.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void DbStorage::LogError(const char* msg)
{
    std::cerr << "DbStorage: " << msg << " - " << (db_ ? sqlite3_errmsg(db_) : "No DB") << std::endl;
}

} // namespace jiuwen

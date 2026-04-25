#include "src/context_engine/db_storage.h"
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "third_party/include/sqlite3.h"

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
    const char* sql = "CREATE TABLE IF NOT EXISTS messages ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "session_id TEXT NOT NULL, "
                      "role TEXT NOT NULL, "
                      "content TEXT NOT NULL, "
                      "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP"
                      ");";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, 0, &errMsg);
    if (rc != SQLITE_OK) {
        LogError("Failed to create table");
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool DbStorage::SaveMessage(const Message& msg)
{
    if (!IsValidMessage(msg)) return true;
    if (!db_) return false;

    const char* sql = "INSERT INTO messages (session_id, role, content) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sessionId_.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, msg.role.c_str(), -1, SQLITE_STATIC);
        std::string cleanContent = CleanMessageContent(msg.content);
        sqlite3_bind_text(stmt, 3, cleanContent.c_str(), -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return true;
        }
        sqlite3_finalize(stmt);
    }
    LogError("Failed to save message");
    return false;
}

bool DbStorage::LoadHistory(std::vector<Message>& outMessages)
{
    if (!db_) return false;

    const char* sql = "SELECT role, content FROM messages WHERE session_id = ? ORDER BY id;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sessionId_.c_str(), -1, SQLITE_STATIC);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Message msg;
            const unsigned char* role = sqlite3_column_text(stmt, 0);
            const unsigned char* content = sqlite3_column_text(stmt, 1);

            if (role) msg.role = reinterpret_cast<const char*>(role);
            if (content) msg.content = reinterpret_cast<const char*>(content);

            if (!msg.role.empty() && !msg.content.empty()) {
                outMessages.push_back(msg);
            }
        }
        sqlite3_finalize(stmt);
        return true;
    }
    LogError("Failed to load history");
    return false;
}

void DbStorage::Clear()
{
    if (!db_) return;
    const char* sql = "DELETE FROM messages WHERE session_id = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sessionId_.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void DbStorage::LogError(const char* msg)
{
    std::cerr << "DbStorage: " << msg << " - " << (db_ ? sqlite3_errmsg(db_) : "No DB") << std::endl;
}

} // namespace jiuwen

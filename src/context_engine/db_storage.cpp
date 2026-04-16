

#include "src/context_engine/db_storage.h"
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "src/3rd-party/include/sqlite3.h"

DbStorage::DbStorage(const std::string& path, const std::string& sessionId)
    : sessionId_(sessionId)
{
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        PrintError("Failed to open database");
        db_ = nullptr;
    } else {
        CreateTable();
    }
}

DbStorage::~DbStorage()
{
    if (db_) sqlite3_close(db_);
}

bool DbStorage::SaveMessage(const Message& msg)
{
    if (!db_) return false;
    const char* sql = "INSERT INTO messages (session_id, role, content) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sessionId_.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, msg.role.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, msg.content.c_str(), -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return true;
        }
        sqlite3_finalize(stmt);
    }
    PrintError("Failed to save message");
    return false;
}

bool DbStorage::LoadHistory(std::vector<Message>& outMessages)
{
    if (!db_) return false;
    const char* sql = "SELECT role, content FROM messages WHERE session_id = ? ORDER BY rowid;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sessionId_.c_str(), -1, SQLITE_STATIC);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Message msg;
            const unsigned char* role = sqlite3_column_text(stmt, 0);
            const unsigned char* content = sqlite3_column_text(stmt, 1);

            if (role) msg.role = reinterpret_cast<const char*>(role);
            if (content) msg.content = reinterpret_cast<const char*>(content);

            outMessages.push_back(msg);
        }
        sqlite3_finalize(stmt);
        return true;
    }
    PrintError("Failed to load history");
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

bool DbStorage::CreateTable()
{
    const char* sql = "CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL, role TEXT, content TEXT);";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, 0, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "SQL Error: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

void DbStorage::PrintError(const char* msg)
{
    std::cerr << "DbStorage: " << msg << " - " << (db_ ? sqlite3_errmsg(db_) : "No DB") << std::endl;
}

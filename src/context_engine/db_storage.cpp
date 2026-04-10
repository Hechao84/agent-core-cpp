#include "db_storage.h"
#include "sqlite3.h"
#include <iostream>
#include <sstream>

DbStorage::DbStorage(const std::string& path, const std::string& sessionId) 
    : m_sessionId(sessionId) {
    if (sqlite3_open(path.c_str(), &m_db) != SQLITE_OK) {
        PrintError("Failed to open database");
        m_db = nullptr;
    } else {
        CreateTable();
    }
}

DbStorage::~DbStorage() {
    if (m_db) sqlite3_close(m_db);
}

bool DbStorage::SaveMessage(const Message& msg) {
    if (!m_db) return false;
    const char* sql = "INSERT INTO messages (session_id, role, content) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, m_sessionId.c_str(), -1, SQLITE_STATIC);
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

bool DbStorage::LoadHistory(std::vector<Message>& outMessages) {
    if (!m_db) return false;
    const char* sql = "SELECT role, content FROM messages WHERE session_id = ? ORDER BY rowid;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, m_sessionId.c_str(), -1, SQLITE_STATIC);
        
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

void DbStorage::Clear() {
    if (!m_db) return;
    const char* sql = "DELETE FROM messages WHERE session_id = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, m_sessionId.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

bool DbStorage::CreateTable() {
    const char* sql = "CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL, role TEXT, content TEXT);";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, 0, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "SQL Error: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

void DbStorage::PrintError(const char* msg) {
    std::cerr << "DbStorage: " << msg << " - " << (m_db ? sqlite3_errmsg(m_db) : "No DB") << std::endl;
}

#pragma once
#include "storage_interface.h"
#include <string>
#include <vector>

struct sqlite3; // Forward declaration

class DbStorage : public ContextStorageInterface {
public:
    DbStorage(const std::string& connStr, const std::string& sessionId);
    ~DbStorage() override;

    bool SaveMessage(const Message& msg) override;
    bool LoadHistory(std::vector<Message>& outMessages) override;
    void Clear() override;

private:
    sqlite3* m_db = nullptr;
    std::string m_sessionId;
    
    bool CreateTable();
    void PrintError(const char* msg);
};

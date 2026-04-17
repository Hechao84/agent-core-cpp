#pragma once

#include <string>
#include <vector>
#include "src/context_engine/storage_base.h"

struct sqlite3; // Forward declaration

class DbStorage : public ContextStorageBase {
public:
    DbStorage(const std::string& dbPath, const std::string& sessionId);
    ~DbStorage() override;

    bool SaveMessage(const Message& msg) override;
    bool LoadHistory(std::vector<Message>& outMessages) override;
    void Clear() override;

private:
    sqlite3* db_ = nullptr;

    bool InitDatabase();
    bool CreateTable();
    void LogError(const char* msg);
};

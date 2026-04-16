#pragma once

#include <string>
#include <vector>
#include "src/context_engine/storage_interface.h"

struct sqlite3; // Forward declaration

class DbStorage : public ContextStorageInterface {
public:
    DbStorage(const std::string& connStr, const std::string& sessionId);
    ~DbStorage() override;

    bool SaveMessage(const Message& msg) override;
    bool LoadHistory(std::vector<Message>& outMessages) override;
    void Clear() override;

private:
    sqlite3* db_ = nullptr;
    std::string sessionId_;

    bool CreateTable();
    void PrintError(const char* msg);
};

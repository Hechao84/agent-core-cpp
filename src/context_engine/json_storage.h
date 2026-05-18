#pragma once

#include <string>
#include <vector>
#include "src/context_engine/storage_base.h"

namespace jiuwen {

class JsonStorage : public ContextStorageBase {
public:
    JsonStorage(const std::string& path, const std::string& sessionId);

    bool SaveMessage(const Message& msg) override;
    bool LoadHistory(std::vector<Message>& outMessages) override;
    void Clear() override;

private:
    std::string filePath_;
};

} // namespace jiuwen

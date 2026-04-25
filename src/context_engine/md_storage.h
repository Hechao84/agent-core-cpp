#pragma once

#include <string>
#include <vector>
#include "src/context_engine/storage_base.h"

namespace jiuwen {

class MarkdownStorage : public ContextStorageBase {
public:
    MarkdownStorage(const std::string& path, const std::string& sessionId);

    bool SaveMessage(const Message& msg) override;
    bool LoadHistory(std::vector<Message>& outMessages) override;
    void Clear() override;

private:
    std::string filePath_;
    std::string FormatMessage(const Message& msg) const;
    Message ParseMessageBlock(const std::string& block) const;
};

} // namespace jiuwen

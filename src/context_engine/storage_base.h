#pragma once

#include <string>
#include <vector>
#include "include/model.h"

class ContextStorageBase {
public:
    explicit ContextStorageBase(const std::string& sessionId);
    virtual ~ContextStorageBase() = default;

    virtual bool SaveMessage(const Message& msg) = 0;
    virtual bool LoadHistory(std::vector<Message>& outMessages) = 0;
    virtual void Clear() = 0;

protected:
    std::string sessionId_;

    static bool IsValidMessage(const Message& msg);
    static std::string CleanMessageContent(const std::string& input);
};

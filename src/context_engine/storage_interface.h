#pragma once

#include <string>
#include <vector>
#include "include/model.h"

class ContextStorageInterface 
{
public:
    virtual ~ContextStorageInterface() = default;
    virtual bool SaveMessage(const Message& msg) = 0;
    virtual bool LoadHistory(std::vector<Message>& outMessages) = 0;
    virtual void Clear() = 0;
};

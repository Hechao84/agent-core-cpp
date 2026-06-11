#pragma once

#include <string>

#include "include/agent_export.h"
#include "include/memory_runtime.h"
#include "include/memory_types.h"
#include "include/model.h"

namespace jiuwen {

class AGENT_API MemoryAdapter
{
public:
    virtual ~MemoryAdapter() = default;

    virtual bool OnSessionStarted(const std::string& agentId, const std::string& sessionId) = 0;
    virtual bool OnSessionEnded(const std::string& agentId, const std::string& sessionId) = 0;
    virtual bool OnMessageAppended(const MemoryEvent& event) = 0;
    virtual Message ProcessToolResult(const MemoryEvent& event, const Message& toolMessage) = 0;
    virtual MemoryContextPackage BuildContext(const MemoryContextRequest& request) = 0;
    virtual MemoryPayloadWriteResult WritePayload(const MemoryPayloadWriteRequest& request) = 0;
    virtual std::string ReadPayload(const std::string& ref) = 0;
    virtual bool Consolidate(const MemoryConsolidationRequest& request) = 0;
    virtual MemoryStats GetStats() const = 0;
};

} // namespace jiuwen

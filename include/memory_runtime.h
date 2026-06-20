#pragma once

#include <string>
#include <utility>
#include <vector>

#include "include/agent_export.h"
#include "include/memory_types.h"
#include "include/types.h"

namespace jiuwen {

class Model;

class AGENT_API MemoryRuntime
{
public:
    explicit MemoryRuntime(MemoryConfig config) : config_(std::move(config)) {}
    virtual ~MemoryRuntime() = default;

    virtual bool AppendEvent(const MemoryEvent& event) = 0;
    virtual MemoryContextPackage BuildContext(const MemoryContextRequest& request) = 0;
    virtual MemoryPayloadWriteResult WritePayload(const MemoryPayloadWriteRequest& request) = 0;
    virtual std::string ReadPayload(const std::string& uri) = 0;
    virtual bool Consolidate(const MemoryConsolidationRequest& request) = 0;
    virtual bool Consolidate(const MemoryConsolidationRequest& request, Model* model)
    {
        (void)model;
        return Consolidate(request);
    }
    virtual std::vector<MemorySearchHit> SearchMemory(const MemorySearchRequest& request) = 0;
    virtual MemoryStats GetStats() const = 0;

    MemoryConfig GetConfig() const { return config_; }

protected:
    MemoryConfig config_;
};

} // namespace jiuwen

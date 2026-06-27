#pragma once

#include <string>
#include <utility>
#include <vector>

#include "include/agent_export.h"
#include "include/memory_config.h"
#include "include/memory_types.h"

namespace jiuwen {

class AGENT_API MemoryRuntime
{
public:
    explicit MemoryRuntime(MemoryConfig config) : config_(std::move(config)) {}
    virtual ~MemoryRuntime() = default;

    virtual bool AppendEvent(const MemoryEvent& event) = 0;
    virtual MemoryContextPackage BuildContext(const MemoryContextRequest& request) = 0;
    virtual MemoryPayloadWriteResult WritePayload(const MemoryPayloadWriteRequest& request) = 0;
    virtual std::string ReadPayload(const std::string& uri) = 0;

    // Consolidation has two overloads mirroring agent-memory-cpp's interface:
    //
    // 1) Consolidate(request): the runtime decides the model source — it uses
    //    its own configured model when available, otherwise falls back to
    //    rule-based extraction. HTTP-backed runtimes consolidate on the server
    //    side using the server's configured model.
    //
    // 2) Consolidate(request, model): use only the explicitly provided model.
    //    Passing nullptr explicitly disables model use. The model must remain
    //    valid for the duration of the call. HTTP-backed runtimes ignore the
    //    model parameter (the model lives in the client process and cannot be
    //    used by the remote server) and behave like overload (1).
    virtual bool Consolidate(const MemoryConsolidationRequest& request) = 0;
    virtual bool Consolidate(const MemoryConsolidationRequest& request,
                             MemoryModelClient* modelClient) = 0;
    virtual std::vector<MemorySearchHit> SearchMemory(const MemorySearchRequest& request) = 0;
    virtual MemoryStats GetStats() const = 0;

    MemoryConfig GetConfig() const { return config_; }

protected:
    MemoryConfig config_;
};

} // namespace jiuwen

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "include/memory_runtime.h"

namespace agent_memory {
class BuiltinMemoryRuntime;
}

namespace jiuwen {

class BuiltinMemoryRuntime : public MemoryRuntime
{
public:
    explicit BuiltinMemoryRuntime(MemoryConfig config);
    ~BuiltinMemoryRuntime() override;

    bool AppendEvent(const MemoryEvent& event) override;
    MemoryContextPackage BuildContext(const MemoryContextRequest& request) override;
    MemoryPayloadWriteResult WritePayload(const MemoryPayloadWriteRequest& request) override;
    std::string ReadPayload(const std::string& uri) override;
    bool Consolidate(const MemoryConsolidationRequest& request) override;
    bool Consolidate(const MemoryConsolidationRequest& request, MemoryModelClient* modelClient) override;
    std::vector<MemorySearchHit> SearchMemory(const MemorySearchRequest& request) override;
    MemoryStats GetStats() const override;

private:
    std::unique_ptr<agent_memory::BuiltinMemoryRuntime> impl_;
    mutable std::atomic<int> appendFailures_{0};
    mutable std::atomic<int> writeFailures_{0};
    mutable std::atomic<int> buildContextFailures_{0};
};

} // namespace jiuwen

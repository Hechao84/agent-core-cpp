#pragma once

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
    std::string ReadPayload(const std::string& ref) override;
    bool Consolidate(const MemoryConsolidationRequest& request) override;
    bool Consolidate(const MemoryConsolidationRequest& request, Model* model) override;
    std::vector<MemorySearchResult> SearchMemory(const MemorySearchRequest& request) override;
    MemoryStats GetStats() const override;

private:
    std::unique_ptr<agent_memory::BuiltinMemoryRuntime> impl_;
};

} // namespace jiuwen

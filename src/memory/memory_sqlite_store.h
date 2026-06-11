#pragma once

#include <memory>
#include <string>
#include <vector>

#include "include/memory_types.h"

namespace agent_memory {
class MemorySqliteStore;
}

namespace jiuwen {

class MemorySqliteStore
{
public:
    explicit MemorySqliteStore(std::string dbPath);
    ~MemorySqliteStore();

    bool Initialize();
    bool SaveEvent(const MemoryEvent& event);
    bool SavePayload(const MemoryPayloadRef& payload);
    bool SaveSummary(const std::string& agentId, const std::string& sessionId, const std::string& level,
                     const std::string& topic, const std::string& summary, float confidence,
                     const std::vector<std::string>& sourceRefs = {});
    bool SaveEntity(const MemoryEntity& entity);
    bool SaveRelation(const MemoryRelation& relation);
    bool MarkEntityObsolete(const std::string& entityId, const std::string& supersededBy);
    std::string LoadLongTermMemoryText(int limit) const;
    int CountRows(const std::string& tableName) const;

private:
    std::unique_ptr<agent_memory::MemorySqliteStore> impl_;
};

} // namespace jiuwen

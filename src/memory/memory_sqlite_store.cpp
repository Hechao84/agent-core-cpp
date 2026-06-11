#include "src/memory/memory_sqlite_store.h"

#include <utility>

#include "agent_memory/sqlite_store.h"

namespace jiuwen {

MemorySqliteStore::MemorySqliteStore(std::string dbPath)
    : impl_(std::make_unique<agent_memory::MemorySqliteStore>(std::move(dbPath)))
{
}

MemorySqliteStore::~MemorySqliteStore() = default;

bool MemorySqliteStore::Initialize()
{
    return impl_->Initialize();
}

bool MemorySqliteStore::SaveEvent(const MemoryEvent& event)
{
    return impl_->SaveEvent(event);
}

bool MemorySqliteStore::SavePayload(const MemoryPayloadRef& payload)
{
    return impl_->SavePayload(payload);
}

bool MemorySqliteStore::SaveSummary(const std::string& agentId, const std::string& sessionId, const std::string& level,
                                    const std::string& topic, const std::string& summary, float confidence,
                                    const std::vector<std::string>& sourceRefs)
{
    return impl_->SaveSummary(agentId, sessionId, level, topic, summary, confidence, sourceRefs);
}

bool MemorySqliteStore::SaveEntity(const MemoryEntity& entity)
{
    return impl_->SaveEntity(entity);
}

bool MemorySqliteStore::SaveRelation(const MemoryRelation& relation)
{
    return impl_->SaveRelation(relation);
}

bool MemorySqliteStore::MarkEntityObsolete(const std::string& entityId, const std::string& supersededBy)
{
    return impl_->MarkEntityObsolete(entityId, supersededBy);
}

std::string MemorySqliteStore::LoadLongTermMemoryText(int limit) const
{
    return impl_->LoadLongTermMemoryText(limit);
}

int MemorySqliteStore::CountRows(const std::string& tableName) const
{
    return impl_->CountRows(tableName);
}

} // namespace jiuwen

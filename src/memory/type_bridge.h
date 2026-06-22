#pragma once

#include "include/memory_config.h"
#include "include/memory_types.h"

#include "agent_memory/config.h"
#include "agent_memory/context.h"
#include "agent_memory/event.h"
#include "agent_memory/long_term_memory.h"
#include "agent_memory/payload.h"
#include "agent_memory/search.h"
#include "agent_memory/stats.h"

namespace jiuwen {

// Bidirectional value-type conversions between the jiuwen public memory types
// and the agent-memory-cpp types. These live in the BuiltinMemoryRuntime
// compilation unit only, so the rest of the framework stays free of any
// agent_memory:: dependency.

agent_memory::MemoryConfig ToAgentMemoryConfig(const MemoryConfig& cfg);
agent_memory::MemoryEvent ToAgentEvent(const MemoryEvent& event);
agent_memory::MemoryPayloadWriteRequest ToAgentPayloadWriteRequest(const MemoryPayloadWriteRequest& request);
agent_memory::MemoryContextRequest ToAgentContextRequest(const MemoryContextRequest& request);
agent_memory::MemoryConsolidationRequest ToAgentConsolidationRequest(const MemoryConsolidationRequest& request);
agent_memory::MemorySearchRequest ToAgentSearchRequest(const MemorySearchRequest& request);

MemoryContextPackage FromAgentContextPackage(const agent_memory::MemoryContextPackage& pkg);
MemoryPayloadWriteResult FromAgentPayloadWriteResult(const agent_memory::MemoryPayloadWriteResult& result);
MemorySearchHit FromAgentSearchHit(const agent_memory::MemorySearchHit& hit);
MemoryStats FromAgentStats(const agent_memory::MemoryStats& stats);

} // namespace jiuwen

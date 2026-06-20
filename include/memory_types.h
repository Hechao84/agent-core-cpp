#pragma once

#include "agent_memory/config.h"
#include "agent_memory/context.h"
#include "agent_memory/event.h"
#include "agent_memory/long_term_memory.h"
#include "agent_memory/model_client.h"
#include "agent_memory/payload.h"
#include "agent_memory/search.h"
#include "agent_memory/stats.h"

namespace jiuwen {

using MemoryModelConfig = agent_memory::MemoryModelConfig;
using MemoryContextPackage = agent_memory::MemoryContextPackage;
using MemoryContextRequest = agent_memory::MemoryContextRequest;
using MemoryConsolidationRequest = agent_memory::MemoryConsolidationRequest;
using MemoryEntity = agent_memory::MemoryEntity;
using MemoryEvent = agent_memory::MemoryEvent;
using MemoryEventType = agent_memory::MemoryEventType;
using MemoryMessage = agent_memory::MemoryMessage;
using MemoryPayloadRef = agent_memory::MemoryPayloadRef;
using MemoryPayloadWriteRequest = agent_memory::MemoryPayloadWriteRequest;
using MemoryPayloadWriteResult = agent_memory::MemoryPayloadWriteResult;
using MemoryPayloadReadResult = agent_memory::MemoryPayloadReadResult;
using MemoryRelation = agent_memory::MemoryRelation;
using MemorySearchHit = agent_memory::MemorySearchHit;
using MemorySearchRequest = agent_memory::MemorySearchRequest;
using MemoryStats = agent_memory::MemoryStats;
using ModelInvokeResult = agent_memory::ModelInvokeResult;
using MemoryModelClient = agent_memory::MemoryModelClient;

} // namespace jiuwen

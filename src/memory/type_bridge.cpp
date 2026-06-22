#include "src/memory/type_bridge.h"

namespace jiuwen {

namespace {

agent_memory::MemoryEventType ToAgentEventType(MemoryEventType type)
{
    switch (type) {
        case MemoryEventType::SESSION_STARTED: return agent_memory::MemoryEventType::SESSION_STARTED;
        case MemoryEventType::SESSION_ENDED: return agent_memory::MemoryEventType::SESSION_ENDED;
        case MemoryEventType::MESSAGE_APPENDED: return agent_memory::MemoryEventType::MESSAGE_APPENDED;
        case MemoryEventType::TOOL_CALL_STARTED: return agent_memory::MemoryEventType::TOOL_CALL_STARTED;
        case MemoryEventType::TOOL_CALL_FINISHED: return agent_memory::MemoryEventType::TOOL_CALL_FINISHED;
        case MemoryEventType::PAYLOAD_OFFLOADED: return agent_memory::MemoryEventType::PAYLOAD_OFFLOADED;
        case MemoryEventType::CONSOLIDATION_REQUESTED: return agent_memory::MemoryEventType::CONSOLIDATION_REQUESTED;
        case MemoryEventType::CONSOLIDATION_COMPLETED: return agent_memory::MemoryEventType::CONSOLIDATION_COMPLETED;
    }
    return agent_memory::MemoryEventType::MESSAGE_APPENDED;
}

MemoryEntity FromAgentEntity(const agent_memory::MemoryEntity& e)
{
    MemoryEntity out;
    out.id = e.id;
    out.agentId = e.agentId;
    out.entityType = e.entityType;
    out.name = e.name;
    out.summary = e.summary;
    out.confidence = e.confidence;
    out.isActive = e.isActive;
    out.supersededByEntityId = e.supersededByEntityId;
    out.supersededEntityId = e.supersededEntityId;
    out.sourceRefs = e.sourceRefs;
    out.metadata = e.metadata;
    out.createdAt = e.createdAt;
    out.updatedAt = e.updatedAt;
    return out;
}

MemoryRelation FromAgentRelation(const agent_memory::MemoryRelation& r)
{
    MemoryRelation out;
    out.id = r.id;
    out.agentId = r.agentId;
    out.fromEntityId = r.fromEntityId;
    out.relationType = r.relationType;
    out.toEntityId = r.toEntityId;
    out.confidence = r.confidence;
    out.sourceRefs = r.sourceRefs;
    out.metadata = r.metadata;
    out.createdAt = r.createdAt;
    out.updatedAt = r.updatedAt;
    return out;
}

MemoryPayloadRef FromAgentPayloadRef(const agent_memory::MemoryPayloadRef& p)
{
    MemoryPayloadRef out;
    out.agentId = p.agentId;
    out.sessionId = p.sessionId;
    out.uri = p.uri;
    out.contentType = p.contentType;
    out.summary = p.summary;
    out.toolName = p.toolName;
    out.originalChars = p.originalChars;
    out.metadata = p.metadata;
    out.createdAt = p.createdAt;
    return out;
}

} // namespace

agent_memory::MemoryConfig ToAgentMemoryConfig(const MemoryConfig& cfg)
{
    agent_memory::MemoryConfig out;
    out.dataPath = cfg.dataPath;
    out.tokenBudget = cfg.tokenBudget;
    out.offloadThresholdChars = cfg.offloadToolResultChars;
    out.enablePayloadOffload = cfg.enablePayloadOffload;
    out.model.enabled = cfg.modelEnabled;
    out.model.formatType = cfg.modelFormatType;
    out.model.baseUrl = cfg.modelBaseUrl;
    out.model.apiKey = cfg.modelApiKey;
    out.model.modelName = cfg.modelName;
    out.model.organization = cfg.modelOrganization;
    out.model.anthropicVersion = cfg.modelAnthropicVersion;
    out.model.timeoutSeconds = cfg.modelTimeoutSeconds;
    out.model.temperature = cfg.modelTemperature;
    out.model.maxTokens = cfg.modelMaxTokens;
    return out;
}

agent_memory::MemoryEvent ToAgentEvent(const MemoryEvent& event)
{
    agent_memory::MemoryEvent out;
    out.type = ToAgentEventType(event.type);
    out.agentId = event.agentId;
    out.sessionId = event.sessionId;
    out.role = event.role;
    out.content = event.content;
    out.toolCallId = event.toolCallId;
    out.toolName = event.toolName;
    out.payloadRef = event.payloadRef;
    out.storeCursor = event.storeCursor;
    out.metadata = event.metadata;
    out.timestamp = event.timestamp;
    return out;
}

agent_memory::MemoryPayloadWriteRequest ToAgentPayloadWriteRequest(const MemoryPayloadWriteRequest& request)
{
    agent_memory::MemoryPayloadWriteRequest out;
    out.agentId = request.agentId;
    out.sessionId = request.sessionId;
    out.content = request.content;
    out.contentType = request.contentType;
    out.toolCallId = request.toolCallId;
    out.toolName = request.toolName;
    out.metadata = request.metadata;
    return out;
}

agent_memory::MemoryContextRequest ToAgentContextRequest(const MemoryContextRequest& request)
{
    agent_memory::MemoryContextRequest out;
    out.agentId = request.agentId;
    out.sessionId = request.sessionId;
    out.query = request.query;
    out.tokenBudget = request.tokenBudget;
    out.includeSections = request.includeSections;
    out.metadata = request.metadata;
    return out;
}

agent_memory::MemoryConsolidationRequest ToAgentConsolidationRequest(const MemoryConsolidationRequest& request)
{
    agent_memory::MemoryConsolidationRequest out;
    out.agentId = request.agentId;
    out.sessionId = request.sessionId;
    out.maxEvents = request.maxEvents;
    out.forceReprocess = request.forceReprocess;
    out.metadata = request.metadata;
    return out;
}

agent_memory::MemorySearchRequest ToAgentSearchRequest(const MemorySearchRequest& request)
{
    agent_memory::MemorySearchRequest out;
    out.agentId = request.agentId;
    out.sessionId = request.sessionId;
    out.query = request.query;
    out.limit = request.limit;
    out.includeSections = request.includeSections;
    out.metadata = request.metadata;
    return out;
}

MemoryContextPackage FromAgentContextPackage(const agent_memory::MemoryContextPackage& pkg)
{
    MemoryContextPackage out;
    for (const auto& m : pkg.messages) {
        MemoryMessage msg;
        msg.role = m.role;
        msg.content = m.content;
        msg.toolCallId = m.toolCallId;
        msg.toolName = m.toolName;
        msg.payloadRef = m.payloadRef;
        out.messages.push_back(std::move(msg));
    }
    out.memoryText = pkg.memoryText;
    for (const auto& e : pkg.entities) {
        out.entities.push_back(FromAgentEntity(e));
    }
    for (const auto& r : pkg.relations) {
        out.relations.push_back(FromAgentRelation(r));
    }
    for (const auto& p : pkg.payloadRefs) {
        out.payloadRefs.push_back(FromAgentPayloadRef(p));
    }
    out.citations = pkg.citations;
    out.metadata = pkg.metadata;
    return out;
}

MemoryPayloadWriteResult FromAgentPayloadWriteResult(const agent_memory::MemoryPayloadWriteResult& result)
{
    MemoryPayloadWriteResult out;
    out.succeeded = result.succeeded;
    out.offloaded = result.offloaded;
    out.payload = FromAgentPayloadRef(result.payload);
    out.replacementContent = result.replacementContent;
    return out;
}

MemorySearchHit FromAgentSearchHit(const agent_memory::MemorySearchHit& hit)
{
    MemorySearchHit out;
    out.id = hit.id;
    out.type = hit.type;
    out.content = hit.content;
    out.score = hit.score;
    out.sourceRefs = hit.sourceRefs;
    out.metadata = hit.metadata;
    return out;
}

MemoryStats FromAgentStats(const agent_memory::MemoryStats& stats)
{
    MemoryStats out;
    out.events = stats.events;
    out.payloads = stats.payloads;
    out.summaries = stats.summaries;
    out.entities = stats.entities;
    out.relations = stats.relations;
    out.metadata = stats.metadata;
    return out;
}

} // namespace jiuwen

#pragma once

#include <string>
#include <vector>

#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

// Event kinds that can be appended to short-term memory.
enum class MemoryEventType
{
    SESSION_STARTED,
    SESSION_ENDED,
    MESSAGE_APPENDED,
    TOOL_CALL_STARTED,
    TOOL_CALL_FINISHED,
    PAYLOAD_OFFLOADED,
    CONSOLIDATION_REQUESTED,
    CONSOLIDATION_COMPLETED,
};

// Short-term event stored as the source stream for context and consolidation.
struct MemoryEvent
{
    MemoryEventType type{MemoryEventType::MESSAGE_APPENDED};
    std::string agentId;
    std::string sessionId;
    std::string role;
    std::string content;
    std::string toolCallId;
    std::string toolName;
    std::string payloadRef;
    std::string storeCursor;
    nlohmann::json metadata = nlohmann::json::object();
    std::string timestamp;
};

// Message entry included in a context package.
struct MemoryMessage
{
    std::string role;
    std::string content;
    std::string toolCallId;
    std::string toolName;
    std::string payloadRef;
};

// Long-term memory entity extracted from event history.
struct MemoryEntity
{
    std::string id;
    std::string agentId;
    std::string entityType;
    std::string name;
    std::string summary;
    float confidence{0.0F};
    bool isActive{true};
    std::string supersededByEntityId;
    std::string supersededEntityId;
    std::vector<std::string> sourceRefs;
    nlohmann::json metadata = nlohmann::json::object();
    std::string createdAt;
    std::string updatedAt;
};

// Relationship between two long-term memory entities.
struct MemoryRelation
{
    std::string id;
    std::string agentId;
    std::string fromEntityId;
    std::string relationType;
    std::string toEntityId;
    float confidence{0.0F};
    std::vector<std::string> sourceRefs;
    nlohmann::json metadata = nlohmann::json::object();
    std::string createdAt;
    std::string updatedAt;
};

// Reference to an offloaded payload stored outside the event stream.
struct MemoryPayloadRef
{
    std::string agentId;
    std::string sessionId;
    std::string uri;
    std::string contentType;
    std::string summary;
    std::string toolName;
    int originalChars{0};
    nlohmann::json metadata = nlohmann::json::object();
    std::string createdAt;
};

// Request to write or offload a large payload.
struct MemoryPayloadWriteRequest
{
    std::string agentId;
    std::string sessionId;
    std::string content;
    std::string contentType;
    std::string toolCallId;
    std::string toolName;
    nlohmann::json metadata = nlohmann::json::object();
};

// Result of writing or offloading a payload.
struct MemoryPayloadWriteResult
{
    bool succeeded{false};
    bool offloaded{false};
    MemoryPayloadRef payload;
    std::string replacementContent;

    explicit operator bool() const { return succeeded; }
};

// Result of reading an offloaded payload.
struct MemoryPayloadReadResult
{
    bool succeeded{false};
    std::string content;

    explicit operator bool() const { return succeeded; }
};

// Request for building an agent context package.
struct MemoryContextRequest
{
    std::string agentId;
    std::string sessionId;
    std::string query;
    int tokenBudget{4096};
    std::vector<std::string> includeSections;
    nlohmann::json metadata = nlohmann::json::object();
};

// Context package assembled from recent messages, long-term memory, and payload refs.
struct MemoryContextPackage
{
    std::vector<MemoryMessage> messages;
    std::string memoryText;
    std::vector<MemoryEntity> entities;
    std::vector<MemoryRelation> relations;
    std::vector<MemoryPayloadRef> payloadRefs;
    std::vector<std::string> citations;
    nlohmann::json metadata = nlohmann::json::object();
};

// Request to consolidate short-term events into long-term memory.
struct MemoryConsolidationRequest
{
    std::string agentId;
    std::string sessionId;
    int maxEvents{100};
    bool forceReprocess{false};
    nlohmann::json metadata = nlohmann::json::object();
};

// Single long-term memory search hit.
struct MemorySearchHit
{
    std::string id;
    std::string type;
    std::string content;
    float score{0.0F};
    std::vector<std::string> sourceRefs;
    nlohmann::json metadata = nlohmann::json::object();
};

// Request to search long-term memory.
struct MemorySearchRequest
{
    std::string agentId;
    std::string sessionId;
    std::string query;
    int limit{10};
    std::vector<std::string> includeSections;
    nlohmann::json metadata = nlohmann::json::object();
};

// Runtime Store statistics.
struct MemoryStats
{
    int events{0};
    int payloads{0};
    int summaries{0};
    int entities{0};
    int relations{0};
    nlohmann::json metadata = nlohmann::json::object();
};

// Configuration for a runtime-owned model client.
struct MemoryModelConfig
{
    bool enabled{false};
    std::string formatType{"openai"};
    std::string baseUrl;
    std::string apiKey;
    std::string modelName;
    std::string organization;
    std::string anthropicVersion{"2023-06-01"};
    int timeoutSeconds{60};
    double temperature{0.0};
    int maxTokens{0};
};

// Result returned by a host-provided model invocation used during consolidation.
struct MemoryModelResult
{
    std::string text;
    int httpStatus{0};
    std::string errorCode;
    std::string errorMessage;
    std::string providerError;

    explicit operator bool() const { return errorMessage.empty() && !text.empty(); }
};

// Minimal model client abstraction the host may supply to a MemoryRuntime for
// LLM-backed consolidation. Plugins depend only on this interface, never on the
// framework's Model class.
class MemoryModelClient
{
public:
    virtual ~MemoryModelClient() = default;

    // Generates a long-term-memory update JSON string from the supplied prompt.
    virtual MemoryModelResult GenerateMemoryUpdate(const std::string& prompt) = 0;
};

} // namespace jiuwen

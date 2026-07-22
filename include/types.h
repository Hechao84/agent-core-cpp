#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "include/config_node.h"
#include "include/memory_config.h"

namespace jiuwen {

enum class ModelFormatType 
{
    OPENAI,
    ANTHROPIC,
};

struct RetryPolicy
{
    int maxRetries{2};       // Number of retries after the first attempt (total attempts = 1 + maxRetries)
    int baseDelayMs{400};    // Base delay for exponential backoff (milliseconds)
    int maxDelayMs{3000};    // Maximum backoff delay cap (milliseconds)
    bool withJitter{true};   // Add random jitter to backoff delay to avoid thundering herd
};

struct ModelConfig 
{
    std::string baseUrl;      // API endpoint address
    std::string apiKey;       // API authentication key
    std::string modelName;    // Specific model name (e.g., gpt-4o, claude-3-opus, ark-code-latest)
    
    // Provider: Model provider identifier (optional)
    // - Empty string: Use standard implementation for formatType
    // - Non-empty: Prefer custom implementation registered for this provider
    // Purpose: Handle subtle protocol differences from specific vendors
    // (e.g., some OpenAI-compatible APIs don't support role=tool)
    std::string provider;
    
    // FormatType: API protocol format
    // - Determines request/response serialization format
    // - Any OpenAI-compatible service can use ModelFormatType::OPENAI
    ModelFormatType formatType{ModelFormatType::OPENAI};

    // Use native function-calling protocol (tools[] + tool_calls + tool_call_id)
    // when supported by the provider. Set to false to fall back to prompt-only
    // ReAct (the model is asked to print a JSON tool-call which the framework
    // parses out of plain text). Default: true.
    bool useNativeFunctionCalling{true};

    // Extended parameters supporting hierarchy (e.g., "model.temperature")
    // Uses std::variant instead of std::any for type safety.
    ConfigNode extraParams;

    // Retry policy for transient model invocation errors (HTTP 429/5xx,
    // curl timeout/connection failure). Not applied to permanent errors
    // (HTTP 4xx except 429, auth failure, parse errors). The retry loop
    // runs inside Model::Invoke; CallModelStream does not retry on its own.
    RetryPolicy retryPolicy;
};

enum class AgentWorkMode 
{
    REACT,             // Implemented
    PLAN_AND_EXECUTE,  // Reserved, not implemented (factory throws)
    WORKFLOW,          // Reserved, not implemented (factory throws)
};

// Prompt Resource Type
enum class PromptResourceType 
{
    TEXT,
    FILE_PATH
};

// Prompt Resource: Represents a prompt snippet, either inline text or a file to be loaded
struct PromptResource 
{
    PromptResourceType type{PromptResourceType::TEXT};
    std::string value;
};

// Context Engine Configuration
struct ContextConfig 
{
    int maxContextTokens{4096};
    int maxMessages{50};
    std::string sessionId;
    std::string storagePath; // For DB: path to file, For File: directory path
    enum class StorageType{ MEMORY_ONLY, JSON_FILE, DATABASE };
    StorageType storageType{StorageType::JSON_FILE}; // Default to Json file for persistence
    bool enableSummarization{false};
    // idleConsolidationSeconds has migrated to MemoryConfig (it is a
    // memory-policy knob, not context-engine state). JSON deserialization
    // still falls back to contextConfig.idleConsolidationSeconds for
    // backward compatibility with older config files.
};

// Session-specific configuration (per-session agent runtime)
struct SessionConfig
{
    std::string sessionId;
    std::map<std::string, std::string> metadata; // channel, sender, etc.

    // Per-session agent overrides (fallback to global defaults)
    int maxIterations{0}; // 0 = use global default
};

// Session invocation result
struct SessionInvokeResult
{
    std::string sessionId;
    bool success{true};
    std::string errorMessage;
    std::string content;
};

struct HistoryEntry
{
    int cursor;
    std::string timestamp;
    std::string role;
    std::string content;
    std::string toolCallId;
    std::string toolName;
    std::string payloadRef;
    int toolsUsed{0};
};

struct DreamConfig
{
    std::string dataBasePath;
    std::string historyPath;
    int maxBatchSize{20};
    int maxIterations{10};
    int maxToolResultChars{16000};
    int historyEntryPreviewMaxChars{4000};
    int memoryFileMaxChars{32000};
};

struct McpServerConfig
{
    std::string id;
    std::string name;
    std::string description;
    bool enabled{true};
    // Transport config
    std::string type;  // "streamable-http-client", "stdio", "sse"
    std::string url;
    std::string endpoint;
    std::string command;  // for stdio
    std::vector<std::string> args;  // for stdio
    std::map<std::string, std::string> env;
    std::map<std::string, std::string> headers;
    // HTTP timeouts in seconds (Streamable HTTP / SSE transport). 0 = leave
    // curl defaults. Defaults preserve the previous hardcoded 3s/10s behavior.
    int connectTimeoutSeconds{3};
    int requestTimeoutSeconds{10};
};

// Progressive capability disclosure mode. Controls whether tool schemas
// are fully resident in the prompt/FC array (disabled = current behavior)
// or progressively disclosed via a Tier 1 name+description catalog plus a
// per-turn tool_search/load mechanism (progressive/selective). `auto` is a
// placeholder for future budget-driven selection and currently maps to
// `disabled` with a warning.
enum class ToolDisclosureMode
{
    DISABLED,     // Small scale: full schemas resident (current behavior)
    PROGRESSIVE,  // Medium scale: Tier 1 catalog + on-demand load
    SELECTIVE,    // Large scale: findRelevant seeds Tier 1 (v2; v1 falls back to progressive)
    AUTO,         // Budget-driven: resolves to DISABLED/PROGRESSIVE/SELECTIVE via ResolveByBudget (§5.4.2)
};

struct AgentConfig 
{
    std::string id;
    std::string name;
    std::string description;
    std::string version;
    AgentWorkMode mode;
    ModelConfig modelConfig;
    std::unordered_map<std::string, PromptResource> promptTemplates;
    ContextConfig contextConfig;
    DreamConfig dreamConfig;
    MemoryConfig memoryConfig;
    std::string skillDirectory;
    int maxIterations{10};

    // Multi-session settings
    std::string dataBasePath; // "./data" - root of all data
    int maxConcurrentSessions{3}; // Global concurrency gate (0 = unlimited)
    std::vector<std::string> defaultTools; // Tools registered for all sessions

    // Progressive capability disclosure (§5.0 of round5 design). Controls
    // whether tool schemas are fully resident (DISABLED) or progressively
    // disclosed via a Tier 1 name+description catalog + on-demand
    // tool_search/load (PROGRESSIVE/SELECTIVE). V1 defaulted to DISABLED as
    // a safe initial value (zero regression). V2 (round5 §5.4.1) implements
    // findRelevant (LLM-backed capability recall via CapabilitySelector), so
    // SELECTIVE is now real and is the new default — large pools trigger
    // turn-start recall to seed the active set, turn-mid search is
    // LLM-backed when the active set is a subset. PROGRESSIVE remains a
    // middle ground (full pool always visible, load on demand). AUTO is
    // still a placeholder (maps to DISABLED + warning until §10 TODO
    // budget-driven selection is implemented).
    //
    // Performance note: SELECTIVE adds +1 LLM call per user turn (the
    // findRelevant call). For small-scale agents (under ~20 tools) where
    // full schemas fit comfortably, configure DISABLED explicitly to skip
    // the +1 call (behavior reverts to V1).
    ToolDisclosureMode toolDisclosureMode{ToolDisclosureMode::SELECTIVE};
    int toolSchemaTokenBudget{0};   // Tier 2 budget hint (consumed by auto's ResolveByBudget §5.4.2; 0 = unused, 该档不参与判定)
    int toolCatalogTokenBudget{0};  // Tier 1 budget hint (consumed by auto's ResolveByBudget §5.4.2; 0 = unused, 该档不参与判定)
    std::vector<std::string> alwaysOnTools; // Extra tools always in FC (besides meta-tools)

    // MCP server ids referenced by this agent (servers are managed by the
    // application layer, e.g. McpServerManager, and registered with
    // ResourceManager::LoadMCPServers at startup).
    std::vector<std::string> mcpServerIds;
};

// Channel message format for web/feishu/telegram routing
struct ChannelMessage
{
    std::string channel; // "websocket", "feishu", "cli"
    std::string chatId;
    std::string senderId;
    std::string sessionId; // Overrides auto-generated session key
    std::string content;
    std::map<std::string, std::string> metadata; // Extra channel-specific data
};

} // namespace jiuwen

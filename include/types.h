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
    int idleConsolidationSeconds{60};
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

#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace jiuwen {

// Forward declaration for recursive configuration structure
struct ConfigNode;

// ConfigValue: A type-safe variant that supports primitives, lists, and nested nodes.
// Replaces std::any to ensure static type safety while supporting hierarchical configuration.
using ConfigValue = std::variant<
    int,
    float,
    bool,
    std::string,
    std::vector<std::string>,
    std::shared_ptr<ConfigNode> // Recursive pointer allows for nested hierarchy
>;

// ConfigNode: Represents a branch or leaf map in the configuration tree
struct ConfigNode 
{
    std::map<std::string, ConfigValue> fields_;

    // Set a value (overwrites if exists)
    void Set(const std::string& key, ConfigValue value) 
    {
        fields_[key] = std::move(value);
    }

    // Set a value using dot-notation path (e.g., "model.temperature") to support hierarchy
    void SetNested(const std::string& path, ConfigValue value) 
    {
        size_t pos = path.find('.');
        if (pos == std::string::npos) {
            fields_[path] = std::move(value);
        } else {
            std::string key = path.substr(0, pos);
            std::string rest = path.substr(pos + 1);

            std::shared_ptr<ConfigNode> node;
            auto it = fields_.find(key);
            if (it != fields_.end()) {
                // Try to cast existing value to Node
                if (auto p = std::get_if<std::shared_ptr<ConfigNode>>(&it->second)) {
                    node = *p;
                }
            }

            // Create node if not exists
            if (!node) {
                node = std::make_shared<ConfigNode>();
                fields_[key] = node;
            }

            node->SetNested(rest, std::move(value));
        }
    }

    // Get pointer to value (returns nullptr if not found or type mismatch)
    template <typename T>
    const T* GetPtr(const std::string& key) const
    {
        auto it = fields_.find(key);
        if (it != fields_.end()) {
            return std::get_if<T>(&(it->second));
        }
        return nullptr;
    }

    // Convenience getter with default value
    template <typename T>
    T GetValue(const std::string& key, T defaultVal) const
    {
        if (const T* val = GetPtr<T>(key)) {
            return *val;
        }
        return defaultVal;
    }
};

enum class ModelFormatType 
{
    OPENAI,
    ANTHROPIC,
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
};

enum class AgentWorkMode 
{
    REACT,
    PLAN_AND_EXECUTE,
    WORKFLOW,
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

struct MemoryConfig
{
    bool enabled{false};
    std::string mode{"sdk"};
    std::string provider{"builtin.compat"};
    std::string dataPath;

    std::string serverUrl;
    std::string serverApiKey;
    int serverTimeoutSeconds{10};

    int tokenBudget{4096};
    int offloadToolResultChars{8000};
    bool enablePayloadOffload{false};

    bool modelEnabled{false};
    std::string modelFormatType{"openai"};
    std::string modelBaseUrl;
    std::string modelApiKey;
    std::string modelName;
    std::string modelOrganization;
    std::string modelAnthropicVersion{"2023-06-01"};
    int modelTimeoutSeconds{60};
    double modelTemperature{0.0};
    int modelMaxTokens{0};

    ConfigNode extraParams;
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

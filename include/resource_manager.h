#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "include/memory_runtime.h"
#include "include/model.h"
#include "include/tool.h"
#include "include/types.h"

namespace jiuwen {

class MCPConnection;
class SessionTodoList;
class AskUserDispatcher;
class SkillEngine;
class TurnState;  // Per-turn capability disclosure state (full def in src/core/turn_state.h; internal, not exported)
class CapabilitySelector;  // LLM-backed capability recall (full def in src/core/capability_selector.h; internal, not exported)

// Context for session-scoped tool construction. When a tool instance is
// created for actual execution (via CreateSessionTool), the pointers are
// populated from WorkerEnv. When a probe instance is created for schema
// extraction only (via GetToolSchema or BuildToolSchemas), all pointer
// fields are nullptr — tool constructors must not dereference these
// pointers, as the schema is determined entirely by name/description/params
// which are set in the Tool base constructor and never depend on runtime
// context.
struct ToolBuildContext {
    SessionTodoList* todoList{nullptr};
    AskUserDispatcher* askUser{nullptr};
    MemoryRuntime* memoryRuntime{nullptr};
    SkillEngine* skillEngine{nullptr};  // Agent-scoped (shared across sessions), non-owning
    // Per-turn capability disclosure state. nullptr in the null-ctx probe
    // path (schema extraction only); populated at runtime by ExecuteTool via
    // workerEnv_->GetCurrentTurnState() pointing at the current SessionEntry's
    // proxy. tool_search uses it for load/search/getActiveSet/getLoadedTools/
    // isActiveFullPool; skill_search uses it for searchSkill/getSkillActiveSet/
    // isActiveFullSkillPool.
    TurnState* turnState{nullptr};
    // LLM-backed capability recall engine (round5 §5.4.1 条 10). nullptr in
    // the null-ctx probe path and under disabled mode (no progressive
    // disclosure active — substring matching runs in skill_search instead).
    // Populated at runtime by ExecuteTool when IsProgressiveDisclosureActive().
    // tool_search/skill_search use it for the real-recall branch (turn-mid
    // search action when isActiveFullPool()/isActiveFullSkillPool() is false).
    CapabilitySelector* capabilitySelector{nullptr};
    std::function<void(const std::string&)> streamCallback;
    std::string sessionId;
};

class AGENT_API ResourceManager {
public:
    static ResourceManager& GetInstance();

    ~ResourceManager();

    void RegisterTool(const std::string& name, std::function<std::unique_ptr<Tool>()> factory);
    void RegisterModel(const std::string& provider, std::function<std::unique_ptr<Model>(const ModelConfig&)> factory);
    void RegisterMemoryRuntime(const std::string& provider,
                               std::function<std::unique_ptr<MemoryRuntime>(const MemoryConfig&)> factory);

    // Scan a directory for memory plugin shared libraries (*.so / *.dll),
    // load each one and invoke its exported RegisterMemoryPlugin(ResourceManager&)
    // entry point so the plugin can register its own MemoryRuntime providers.
    // Missing directories are ignored; load/symbol failures are logged and
    // skipped so one bad plugin cannot abort startup.
    void LoadMemoryPlugins(const std::string& pluginDir);

    // Close all loaded memory plugin handles. Erases only plugin-registered
    // providers (built-in factories are preserved) before dlclose/FreeLibrary
    // to avoid calling into unloaded code. Idempotent; also called by the
    // destructor.
    void UnloadMemoryPlugins();

    // Session-scoped tool registry (X-3): factory receives a ToolBuildContext
    // carrying the per-session dependencies the tool needs. Stateless tools
    // should keep using RegisterTool.
    using SessionToolFactory = std::function<std::unique_ptr<Tool>(const ToolBuildContext&)>;
    void RegisterSessionTool(const std::string& name, SessionToolFactory factory);
    std::unique_ptr<Tool> CreateSessionTool(const std::string& name, const ToolBuildContext& ctx);
    bool HasSessionTool(const std::string& name) const;
    std::vector<std::string> GetAvailableSessionToolNames() const;
    std::unique_ptr<Tool> CreateTool(const std::string& name);
    std::string GetToolSchema(const std::string& name);
    std::unique_ptr<Model> CreateModel(const ModelConfig& config);
    std::unique_ptr<MemoryRuntime> CreateMemoryRuntime(const MemoryConfig& config);
    std::shared_ptr<MCPConnection> GetMCPServer(const std::string& name);

    // Build native function-calling tool schemas for the requested tools.
    // Unknown tool names are silently skipped.
    std::vector<ToolSchema> BuildToolSchemas(const std::vector<std::string>& toolNames);

    // Rendering mode for the Tier 1 name+description catalog (no parameters).
    // NativeFc: single list (the native FC protocol hard-restricts calls to
    //   the FC array, so a flat catalog suffices).
    // PromptMode: split into "directly callable" / "requires tool_search load
    //   first" sections, since prompt-mode has no protocol-level call gate
    //   and the model could otherwise hallucinate a tool_call for a tool it
    //   never loaded.
    enum class CatalogRenderMode { NativeFc, PromptMode };

    // Tier 1 short catalog: name+description only (no parameters) for the
    // names in visibleNames. Description is sourced from the ToolSchema cache
    // (functionCallSchemas_) when present, else probed from Tool::GetDescription()
    // and cached. renderMode selects flat vs two-section layout (see
    // CatalogRenderMode); callableNames (= alwaysOn ∪ loadedTools) is used only
    // in PromptMode to split the catalog. Non-const because the probe path
    // populates functionCallSchemas_ (mirrors GetToolSchema/BuildToolSchemas).
    std::string GetToolCatalog(const std::set<std::string>& visibleNames,
                               CatalogRenderMode renderMode,
                               const std::set<std::string>& callableNames);

    std::vector<std::string> GetAvailableTools() const;
    std::vector<std::string> GetAvailableModels() const;
    std::vector<std::string> GetAvailableMemoryRuntimes() const;
    std::vector<std::string> GetAvailableMCPServers() const;

    bool HasTool(const std::string& name) const;
    bool HasModel(ModelFormatType type) const;
    bool HasModel(const std::string& provider) const;
    bool HasMemoryRuntime(const std::string& provider) const;
    bool HasMCPServer(const std::string& name) const;

    void LoadMCPServers(const std::vector<McpServerConfig>& configs);
    void RegisterMCPServer(const McpServerConfig& config);
    void UnregisterMCPServer(const std::string& id);
    void RemoveMCPServerRecord(const std::string& id);
    void RegisterMcpTool(const std::string& name, std::function<std::unique_ptr<Tool>()> factory);
    void UnregisterMcpTool(const std::string& name);
    std::vector<McpServerConfig> GetMCPServerConfigs() const;
    std::vector<std::string> GetConnectedMCPServerIds() const;
    std::vector<std::string> GetMcpToolNames() const;

private:
    ResourceManager();
    void RegisterBuiltinTools();
    void RegisterBuiltinModels();
    void RegisterModel(ModelFormatType type, std::function<std::unique_ptr<Model>(const ModelConfig&)> factory);

    // Domain-level mutexes (Lock layer L5): tool, model, memory, and MCP
    // server registries are independently protected so they do not block
    // each other. These four locks are NEVER nested with each other.
    mutable std::mutex toolMutex_;    // Lock layer L5 (tool domain)
    mutable std::mutex modelMutex_;   // Lock layer L5 (model domain)
    mutable std::mutex memoryMutex_;  // Lock layer L5 (memory domain)
    mutable std::mutex mcpMutex_;     // Lock layer L5 (MCP domain)

    // Tool domain (toolMutex_)
    std::unordered_map<std::string, std::function<std::unique_ptr<Tool>()>> toolFactories_;
    std::unordered_map<std::string, SessionToolFactory> sessionToolFactories_;
    std::unordered_set<std::string> mcpToolNames_;

    // promptSchemas_ caches the human-readable text produced by
    // Tool::GetSchema(), used in the fallback (prompt-only) mode where
    // tool signatures are embedded into the system prompt via {$tools}.
    std::unordered_map<std::string, std::string> promptSchemas_;

    // functionCallSchemas_ caches the structured ToolSchema (name +
    // description + JSON Schema parameters) used in native
    // function-calling mode where schemas are serialized into the
    // HTTP request body's "tools" field.
    std::unordered_map<std::string, ToolSchema> functionCallSchemas_;

    // Model domain (modelMutex_)
    std::unordered_map<ModelFormatType, std::function<std::unique_ptr<Model>(const ModelConfig&)>> modelFactories_;
    std::unordered_map<std::string, std::function<std::unique_ptr<Model>(const ModelConfig&)>> providerModelFactories_;

    // Memory domain (memoryMutex_)
    std::unordered_map<std::string, std::function<std::unique_ptr<MemoryRuntime>(const MemoryConfig&)>> memoryFactories_;

    // Handles from dlopen/LoadLibraryA, closed on destruction. Stored as
    // void* (HMODULE is pointer-sized) to avoid including windows.h here.
    std::vector<void*> pluginHandles_;
    // Provider names registered by plugins (vs built-in), so
    // UnloadMemoryPlugins can erase only plugin factories.
    std::vector<std::string> pluginProviders_;

    // MCP server domain (mcpMutex_)
    std::unordered_map<std::string, std::shared_ptr<MCPConnection>> mcpServers_;
};

} // namespace jiuwen

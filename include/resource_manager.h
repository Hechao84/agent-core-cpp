#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "include/model.h"
#include "include/tool.h"
#include "include/types.h"

namespace jiuwen {

class MCPConnection;
class SessionTodoList;
class AskUserDispatcher;

struct ToolBuildContext {
    SessionTodoList* todoList{nullptr};
    AskUserDispatcher* askUser{nullptr};
    std::function<void(const std::string&)> streamCallback;
    std::string sessionId;
};

class AGENT_API ResourceManager {
public:
    static ResourceManager& GetInstance();

    void RegisterTool(const std::string& name, std::function<std::unique_ptr<Tool>()> factory);
    void RegisterModel(const std::string& provider, std::function<std::unique_ptr<Model>(const ModelConfig&)> factory);

    // Session-scoped tool registry (X-3): factory receives a ToolBuildContext
    // carrying the per-session dependencies the tool needs. Stateless tools
    // should keep using RegisterTool.
    using SessionToolFactory = std::function<std::unique_ptr<Tool>(const ToolBuildContext&)>;
    void RegisterSessionTool(const std::string& name, SessionToolFactory factory);
    std::unique_ptr<Tool> CreateSessionTool(const std::string& name, const ToolBuildContext& ctx);
    bool HasSessionTool(const std::string& name) const;
    std::vector<std::string> GetAvailableSessionToolNames() const;
    std::string GetSessionToolSchema(const std::string& name);

    std::unique_ptr<Tool> CreateTool(const std::string& name);
    std::string GetToolSchema(const std::string& name);
    std::unique_ptr<Model> CreateModel(const ModelConfig& config);
    std::shared_ptr<MCPConnection> GetMCPServer(const std::string& name);

    // Build native function-calling tool schemas for the requested tools.
    // Stateless tools use a fresh CreateTool instance; session-scoped tools
    // use the provided ToolBuildContext to construct a probe. Unknown tool
    // names are silently skipped (matches the schema-rendering helpers used
    // by Worker).
    std::vector<ToolSchema> BuildToolSchemas(const std::vector<std::string>& toolNames,
                                              const ToolBuildContext& ctx);

    std::vector<std::string> GetAvailableTools() const;
    std::vector<std::string> GetAvailableModels() const;
    std::vector<std::string> GetAvailableMCPServers() const;

    bool HasTool(const std::string& name) const;
    bool HasModel(ModelFormatType type) const;
    bool HasModel(const std::string& provider) const;
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

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::function<std::unique_ptr<Tool>()>> toolFactories_;
    std::unordered_map<std::string, std::string> toolSchemas_;
    std::unordered_map<std::string, SessionToolFactory> sessionToolFactories_;
    std::unordered_map<std::string, std::string> sessionToolSchemas_;

    // Structured (native function-calling) schema cache, built once per tool
    // and evicted when a tool is registered / unregistered.
    std::unordered_map<std::string, ToolSchema> toolSchemaCache_;
    std::unordered_set<std::string> mcpToolNames_;
    std::unordered_map<ModelFormatType, std::function<std::unique_ptr<Model>(const ModelConfig&)>> modelFactories_;
    std::unordered_map<std::string, std::function<std::unique_ptr<Model>(const ModelConfig&)>> providerModelFactories_;
    std::unordered_map<std::string, std::shared_ptr<MCPConnection>> mcpServers_;
};

} // namespace jiuwen

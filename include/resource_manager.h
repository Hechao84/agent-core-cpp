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

class AGENT_API ResourceManager {
public:
    static ResourceManager& GetInstance();

    void RegisterTool(const std::string& name, std::function<std::unique_ptr<Tool>()> factory);
    void RegisterModel(const std::string& provider, std::function<std::unique_ptr<Model>(const ModelConfig&)> factory);

    std::unique_ptr<Tool> CreateTool(const std::string& name);
    std::string GetToolSchema(const std::string& name);
    std::unique_ptr<Model> CreateModel(const ModelConfig& config);
    std::shared_ptr<MCPConnection> GetMCPServer(const std::string& name);

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
    std::unordered_set<std::string> mcpToolNames_;
    std::unordered_map<ModelFormatType, std::function<std::unique_ptr<Model>(const ModelConfig&)>> modelFactories_;
    std::unordered_map<std::string, std::function<std::unique_ptr<Model>(const ModelConfig&)>> providerModelFactories_;
    std::unordered_map<std::string, std::shared_ptr<MCPConnection>> mcpServers_;
};

} // namespace jiuwen

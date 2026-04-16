#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "include/model.h"
#include "include/tool.h"

class MCPServer;

class AGENT_API ResourceManager {
public:
    static ResourceManager& GetInstance();

    void RegisterTool(const std::string& name, std::function<std::unique_ptr<Tool>()> factory);
    void RegisterModel(ModelFormatType type, std::function<std::unique_ptr<Model>(const ModelConfig&)> factory);
    void RegisterMCPServer(const std::string& name, const std::string& jsonConfig);

    std::unique_ptr<Tool> CreateTool(const std::string& name);
    std::unique_ptr<Model> CreateModel(const ModelConfig& config);
    std::shared_ptr<MCPServer> GetMCPServer(const std::string& name);

    std::vector<std::string> GetAvailableTools() const;
    std::vector<std::string> GetAvailableModels() const;
    std::vector<std::string> GetAvailableMCPServers() const;

    bool HasTool(const std::string& name) const;
    bool HasModel(ModelFormatType type) const;
    bool HasMCPServer(const std::string& name) const;

private:
    ResourceManager();
    void RegisterBuiltinTools();
    void RegisterBuiltinModels();

    std::unordered_map<std::string, std::function<std::unique_ptr<Tool>()>> toolFactories_;
    std::unordered_map<ModelFormatType, std::function<std::unique_ptr<Model>(const ModelConfig&)>> modelFactories_;
    std::unordered_map<std::string, std::shared_ptr<MCPServer>> mcpServers_;
};

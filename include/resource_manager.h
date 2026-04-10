#pragma once
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <memory>
#include "tool.h"
#include "model.h"

class MCPServer;

class AGENT_API ResourceManager {
    public:
        static ResourceManager& GetInstance();
        
        void RegisterTool(const std::string& name, std::function<std::unique_ptr<Tool>()> factory);
        void RegisterModel(ModelFormatType type, std::function<std::unique_ptr<Model>(const ModelConfig&)> factory);
        void RegisterMCPServer(const std::string& name, std::shared_ptr<MCPServer> server);
        
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
        void RegisterBuiltinModels();
        
        std::unordered_map<std::string, std::function<std::unique_ptr<Tool>()>> m_toolFactories;
        std::unordered_map<ModelFormatType, std::function<std::unique_ptr<Model>(const ModelConfig&)>> m_modelFactories;
        std::unordered_map<std::string, std::shared_ptr<MCPServer>> m_mcpServers;
};

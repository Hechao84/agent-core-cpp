#include "resource_manager.h"
#include "model.h"
#include "models/openai_model.h"
#include "models/anthropic_model.h"
#include "tools/mcp_tool.h"
#include <algorithm>
#include <stdexcept>

ResourceManager& ResourceManager::GetInstance() {
    static ResourceManager instance;
    return instance;
}

ResourceManager::ResourceManager() {
    RegisterBuiltinModels();
}

void ResourceManager::RegisterBuiltinModels() {
    RegisterModel(ModelFormatType::OPENAI, [](const ModelConfig& cfg) { return std::make_unique<OpenAIModel>(cfg); });
    RegisterModel(ModelFormatType::ANTHROPIC, [](const ModelConfig& cfg) { return std::make_unique<AnthropicModel>(cfg); });
    RegisterModel(ModelFormatType::DEEPSEEK, [](const ModelConfig& cfg) { return std::make_unique<OpenAIModel>(cfg); });
    RegisterModel(ModelFormatType::DASHSCOPE, [](const ModelConfig& cfg) { return std::make_unique<OpenAIModel>(cfg); });
}

void ResourceManager::RegisterTool(const std::string& name, std::function<std::unique_ptr<Tool>()> factory) {
    toolFactories_[name] = std::move(factory);
}

void ResourceManager::RegisterModel(ModelFormatType type, std::function<std::unique_ptr<Model>(const ModelConfig&)> factory) {
    modelFactories_[type] = std::move(factory);
}

void ResourceManager::RegisterMCPServer(const std::string& name, std::shared_ptr<MCPServer> server) {
    mcpServers_[name] = std::move(server);
}

std::unique_ptr<Tool> ResourceManager::CreateTool(const std::string& name) {
    auto it = toolFactories_.find(name);
    if (it != toolFactories_.end()) return it->second();
    throw std::runtime_error("Tool not found: " + name);
}

std::unique_ptr<Model> ResourceManager::CreateModel(const ModelConfig& config) {
    auto it = modelFactories_.find(config.formatType);
    if (it != modelFactories_.end()) return it->second(config);
    throw std::runtime_error("Model format not registered");
}

std::shared_ptr<MCPServer> ResourceManager::GetMCPServer(const std::string& name) {
    auto it = mcpServers_.find(name);
    if (it != mcpServers_.end()) return it->second;
    return nullptr;
}

std::vector<std::string> ResourceManager::GetAvailableTools() const {
    std::vector<std::string> names;
    for (const auto& p : toolFactories_) names.push_back(p.first);
    return names;
}

std::vector<std::string> ResourceManager::GetAvailableModels() const {
    std::vector<std::string> names;
    std::unordered_map<ModelFormatType, std::string> typeMap = {
        {ModelFormatType::OPENAI, "openai"}, {ModelFormatType::ANTHROPIC, "anthropic"},
        {ModelFormatType::DEEPSEEK, "deepseek"}, {ModelFormatType::DASHSCOPE, "dashscope"}
    };
    for (const auto& p : modelFactories_) {
        if (typeMap.count(p.first)) names.push_back(typeMap[p.first]);
    }
    return names;
}

std::vector<std::string> ResourceManager::GetAvailableMCPServers() const {
    std::vector<std::string> names;
    for (const auto& p : mcpServers_) names.push_back(p.first);
    return names;
}

bool ResourceManager::HasTool(const std::string& name) const { return toolFactories_.count(name) > 0; }
bool ResourceManager::HasModel(ModelFormatType type) const { return modelFactories_.count(type) > 0; }
bool ResourceManager::HasMCPServer(const std::string& name) const { return mcpServers_.count(name) > 0; }

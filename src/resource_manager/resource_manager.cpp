#include "resource_manager.h"
#include "model.h"
#include "models/openai_model.h"
#include "models/anthropic_model.h"
#include "tools/mcp_tool.h"

// Builtin Tools
#include "tools/builtin_tools/time_info_tool.h"
#include "tools/builtin_tools/web_search_tool.h"
#include "tools/builtin_tools/web_fetch_tool.h"
#include "tools/builtin_tools/read_file_tool.h"
#include "tools/builtin_tools/write_file_tool.h"
#include "tools/builtin_tools/edit_file_tool.h"
#include "tools/builtin_tools/list_dir_tool.h"
#include "tools/builtin_tools/glob_tool.h"
#include "tools/builtin_tools/grep_tool.h"
#include "tools/builtin_tools/exec_tool.h"
#include "tools/builtin_tools/notebook_edit_tool.h"
#include "tools/builtin_tools/file_state_tool.h"

#include <algorithm>
#include <stdexcept>

ResourceManager& ResourceManager::GetInstance()
{
    static ResourceManager instance;
    return instance;
}

ResourceManager::ResourceManager()
{
    RegisterBuiltinTools();
    RegisterBuiltinModels();
}

void ResourceManager::RegisterBuiltinTools()
{
    RegisterTool("time_info", []() { return std::make_unique<TimeInfoTool>(); });
    RegisterTool("web_search", []() { return std::make_unique<WebSearchTool>(); });
    RegisterTool("web_fetcher", []() { return std::make_unique<WebFetcherTool>(); });
    RegisterTool("read_file", []() { return std::make_unique<ReadFileTool>(); });
    RegisterTool("write_file", []() { return std::make_unique<WriteFileTool>(); });
    RegisterTool("edit_file", []() { return std::make_unique<EditFileTool>(); });
    RegisterTool("list_dir", []() { return std::make_unique<ListDirTool>(); });
    RegisterTool("glob", []() { return std::make_unique<GlobTool>(); });
    RegisterTool("grep", []() { return std::make_unique<GrepTool>(); });
    RegisterTool("exec", []() { return std::make_unique<ExecTool>(); });
    RegisterTool("notebook_edit", []() { return std::make_unique<NotebookEditTool>(); });
    RegisterTool("file_state", []() { return std::make_unique<FileStateTool>(); });
}

void ResourceManager::RegisterBuiltinModels()
{
    RegisterModel(ModelFormatType::OPENAI, [](const ModelConfig& cfg) { return std::make_unique<OpenAIModel>(cfg); });
    RegisterModel(ModelFormatType::ANTHROPIC, [](const ModelConfig& cfg) { return std::make_unique<AnthropicModel>(cfg); });
    RegisterModel(ModelFormatType::DEEPSEEK, [](const ModelConfig& cfg) { return std::make_unique<OpenAIModel>(cfg); });
    RegisterModel(ModelFormatType::DASHSCOPE, [](const ModelConfig& cfg) { return std::make_unique<OpenAIModel>(cfg); });
}

void ResourceManager::RegisterTool(const std::string& name, std::function<std::unique_ptr<Tool>()> factory)
{
    toolFactories_[name] = std::move(factory);
}

void ResourceManager::RegisterModel(ModelFormatType type, std::function<std::unique_ptr<Model>(const ModelConfig&)> factory)
{
    modelFactories_[type] = std::move(factory);
}

void ResourceManager::RegisterMCPServer(const std::string& name, std::shared_ptr<MCPServer> server)
{
    mcpServers_[name] = std::move(server);
}

void ResourceManager::AddMCPServer(const std::string& name, MCPEndpointConfig config)
{
    auto server = std::make_shared<MCPServer>(name, config);
    mcpServers_[name] = server;

    // Connect the server, which initializes the SDK and registers tools
    server->Connect();
}

std::unique_ptr<Tool> ResourceManager::CreateTool(const std::string& name)
{
    auto it = toolFactories_.find(name);
    if (it != toolFactories_.end()) return it->second();
    throw std::runtime_error("Tool not found: " + name);
}

std::unique_ptr<Model> ResourceManager::CreateModel(const ModelConfig& config)
{
    auto it = modelFactories_.find(config.formatType);
    if (it != modelFactories_.end()) return it->second(config);
    throw std::runtime_error("Model format not registered");
}

std::shared_ptr<MCPServer> ResourceManager::GetMCPServer(const std::string& name)
{
    auto it = mcpServers_.find(name);
    if (it != mcpServers_.end()) return it->second;
    return nullptr;
}

std::vector<std::string> ResourceManager::GetAvailableTools() const
{
    std::vector<std::string> names;
    for (const auto& p : toolFactories_) names.push_back(p.first);
    return names;
}

std::vector<std::string> ResourceManager::GetAvailableModels() const
{
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

std::vector<std::string> ResourceManager::GetAvailableMCPServers() const
{
    std::vector<std::string> names;
    for (const auto& p : mcpServers_) names.push_back(p.first);
    return names;
}

bool ResourceManager::HasTool(const std::string& name) const { return toolFactories_.count(name) > 0; }
bool ResourceManager::HasModel(ModelFormatType type) const { return modelFactories_.count(type) > 0; }
bool ResourceManager::HasMCPServer(const std::string& name) const { return mcpServers_.count(name) > 0; }

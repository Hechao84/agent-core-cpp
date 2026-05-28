#include "include/resource_manager.h"
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "src/utils/logger.h"
#include "include/model.h"
#include "src/models/anthropic_model.h"
#include "src/models/openai_model.h"
// Builtin Tools
#include "src/tools/builtin_tools/edit_file_tool.h"
#include "src/tools/builtin_tools/exec_tool.h"
#include "src/tools/builtin_tools/file_state_tool.h"
#include "src/tools/builtin_tools/glob_tool.h"
#include "src/tools/builtin_tools/grep_tool.h"
#include "src/tools/builtin_tools/list_dir_tool.h"
#include "src/tools/builtin_tools/read_file_tool.h"
#include "src/tools/builtin_tools/skill_search_tool.h"
#include "src/tools/builtin_tools/time_info_tool.h"
#include "src/tools/builtin_tools/web_fetch_tool.h"
#include "src/tools/builtin_tools/web_search_tool.h"
#include "src/tools/builtin_tools/write_file_tool.h"
#include "src/tools/mcp_tool.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

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
    RegisterTool("file_state", []() { return std::make_unique<FileStateTool>(); });
    RegisterTool("list_dir", []() { return std::make_unique<ListDirTool>(); });
    RegisterTool("glob", []() { return std::make_unique<GlobTool>(); });
    RegisterTool("grep", []() { return std::make_unique<GrepTool>(); });
    RegisterTool("exec", []() { return std::make_unique<ExecTool>(); });
    RegisterTool("skill_search", []() { return std::make_unique<SkillSearchTool>(); });
}

void ResourceManager::RegisterBuiltinModels()
{
    RegisterModel(ModelFormatType::OPENAI, 
        [](const ModelConfig& cfg) { return std::make_unique<OpenAIModel>(cfg); });
    RegisterModel(ModelFormatType::ANTHROPIC, 
        [](const ModelConfig& cfg) { return std::make_unique<AnthropicModel>(cfg); });
}

void ResourceManager::RegisterTool(const std::string& name, std::function<std::unique_ptr<Tool>()> factory)
{
    std::lock_guard<std::mutex> lock(mutex_);
    toolFactories_[name] = std::move(factory);
    toolSchemas_.erase(name);
}

void ResourceManager::RegisterModel(ModelFormatType type, std::function<std::unique_ptr<Model>(const ModelConfig&)> factory)
{
    std::lock_guard<std::mutex> lock(mutex_);
    modelFactories_[type] = std::move(factory);
}

void ResourceManager::RegisterModel(const std::string& provider, std::function<std::unique_ptr<Model>(const ModelConfig&)> factory)
{
    std::lock_guard<std::mutex> lock(mutex_);
    providerModelFactories_[provider] = std::move(factory);
}

void ResourceManager::RegisterMCPServer(const std::string& name, const std::string& jsonConfig)
{
    MCPEndpointConfig endpointCfg;
    nlohmann::json config;

    try {
        config = nlohmann::json::parse(jsonConfig);
    } catch (const std::exception& e) {
        throw std::runtime_error("Invalid MCP JSON config: " + std::string(e.what()));
    }

    std::string baseUrl = config.value("url", "");
    std::string endpoint = config.value("endpoint", "");

    // Combine URL and endpoint
    if (baseUrl.empty()) {
        baseUrl = endpoint;
    } else if (!endpoint.empty()) {
        if (baseUrl.back() != '/' && endpoint.front() != '/') {
            baseUrl += "/";
        }
        baseUrl += endpoint;
    }

    endpointCfg.url = baseUrl;

    std::string transportTypeStr = config.value("type", "");
    if (transportTypeStr.empty()) {
        transportTypeStr = config.value("transport", "");
    }

    if (!endpointCfg.url.empty()) {
        if (transportTypeStr == "sse" ||
            (transportTypeStr.empty() && endpointCfg.url.find("/sse") != std::string::npos)) {
            endpointCfg.transportType = MCPTransportType::SSE;
        } else {
            endpointCfg.transportType = MCPTransportType::STREAMABLE_HTTP;
        }

        if (config.contains("headers") && config["headers"].is_object()) {
            for (auto& [k, v] : config["headers"].items()) {
                if (v.is_string()) {
                    endpointCfg.headers[k] = v.get<std::string>();
                }
            }
        }
    } else if (config.contains("command")) {
        endpointCfg.transportType = MCPTransportType::STDIO;
        endpointCfg.command = config["command"].get<std::string>();

        if (config.contains("args") && config["args"].is_array()) {
            for (const auto& arg : config["args"]) {
                if (arg.is_string()) endpointCfg.args.push_back(arg.get<std::string>());
            }
        }

        if (config.contains("env") && config["env"].is_object()) {
            for (auto& [k, v] : config["env"].items()) {
                if (v.is_string()) endpointCfg.env[k] = v.get<std::string>();
            }
        }
    } else {
        throw std::runtime_error("Invalid MCP server config: missing 'url' or 'command'");
    }

    auto server = std::make_shared<MCPServer>(name, endpointCfg);

    // Connect immediately to initialize handshake and discover tools
    server->Connect();

    // Register after connection to avoid race
    std::lock_guard<std::mutex> lock(mutex_);
    mcpServers_[name] = server;
}

std::unique_ptr<Tool> ResourceManager::CreateTool(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    LOG(INFO) << "Creating tool instance: " << name;
    auto it = toolFactories_.find(name);
    if (it != toolFactories_.end()) return it->second();
    LOG(INFO) << "Tool not found in factories: " << name;
    throw std::runtime_error("Tool not found: " + name);
}

std::string ResourceManager::GetToolSchema(const std::string& name)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = toolSchemas_.find(name);
        if (it != toolSchemas_.end()) return it->second;
    }

    auto tool = CreateTool(name);
    std::string schema = tool->GetSchema();
    std::lock_guard<std::mutex> lock(mutex_);
    toolSchemas_[name] = schema;
    return schema;
}

std::unique_ptr<Model> ResourceManager::CreateModel(const ModelConfig& config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 1. First match custom provider implementation (for vendor-specific behavior)
    if (!config.provider.empty()) {
        auto it = providerModelFactories_.find(config.provider);
        if (it != providerModelFactories_.end()) return it->second(config);
    }
    
    // 2. Fall back to standard format implementation
    auto it = modelFactories_.find(config.formatType);
    if (it != modelFactories_.end()) return it->second(config);
    throw std::runtime_error("Model format not registered: use ModelFormatType or register custom provider");
}

std::shared_ptr<MCPServer> ResourceManager::GetMCPServer(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = mcpServers_.find(name);
    if (it != mcpServers_.end()) return it->second;
    return nullptr;
}

std::vector<std::string> ResourceManager::GetAvailableTools() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& p : toolFactories_) names.push_back(p.first);
    return names;
}

std::vector<std::string> ResourceManager::GetAvailableModels() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    std::unordered_map<ModelFormatType, std::string> typeMap = {
        {ModelFormatType::OPENAI, "openai"},
        {ModelFormatType::ANTHROPIC, "anthropic"}
    };
    for (const auto& p : modelFactories_) {
        if (typeMap.count(p.first)) names.push_back(typeMap[p.first]);
    }
    for (const auto& p : providerModelFactories_) {
        names.push_back(p.first);
    }
    return names;
}

std::vector<std::string> ResourceManager::GetAvailableMCPServers() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& p : mcpServers_) names.push_back(p.first);
    return names;
}

bool ResourceManager::HasTool(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return toolFactories_.count(name) > 0;
}

bool ResourceManager::HasModel(ModelFormatType type) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return modelFactories_.count(type) > 0;
}

bool ResourceManager::HasModel(const std::string& provider) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return providerModelFactories_.count(provider) > 0;
}

bool ResourceManager::HasMCPServer(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return mcpServers_.count(name) > 0;
}

} // namespace jiuwen

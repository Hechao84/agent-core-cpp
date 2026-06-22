#include "include/resource_manager.h"
#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "include/memory_runtime.h"
#include "include/model.h"
#include "src/models/anthropic_model.h"
#include "src/models/openai_model.h"
// Builtin Tools
#include "src/tools/builtin_tools/ask_user_tool.h"
#include "src/tools/builtin_tools/edit_file_tool.h"
#include "src/tools/builtin_tools/exec_tool.h"
#include "src/tools/builtin_tools/file_state_tool.h"
#include "src/tools/builtin_tools/glob_tool.h"
#include "src/tools/builtin_tools/grep_tool.h"
#include "src/tools/builtin_tools/list_dir_tool.h"
#include "src/tools/builtin_tools/memory_read_payload_tool.h"
#include "src/tools/builtin_tools/read_file_tool.h"
#include "src/tools/builtin_tools/skill_search_tool.h"
#include "src/tools/builtin_tools/time_info_tool.h"
#include "src/tools/builtin_tools/todo_tool.h"
#include "src/tools/builtin_tools/web_fetch_tool.h"
#include "src/tools/builtin_tools/web_search_tool.h"
#include "src/tools/builtin_tools/write_file_tool.h"
#include "src/memory/http_memory_runtime.h"
#include "src/mcp/mcp_config_manager.h"
#include "src/mcp/mcp_connection.h"
#include "src/utils/logger.h"
#include "third_party/include/nlohmann/json.hpp"

#ifdef JIUWEN_ENABLE_MEMORY_BUILTIN
#include "src/memory/builtin_memory_runtime.h"
#endif

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
#ifdef JIUWEN_ENABLE_MEMORY_BUILTIN
    RegisterMemoryRuntime("builtin.compat", [](const MemoryConfig& cfg) {
        return std::make_unique<BuiltinMemoryRuntime>(cfg);
    });
#endif
    RegisterMemoryRuntime("http.server", [](const MemoryConfig& cfg) {
        return std::make_unique<HttpMemoryRuntime>(cfg);
    });
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

    // Session-scoped builtin tools: Todo + AskUser. These need per-session
    // resources, so they are constructed via the SessionToolFactory path.
    RegisterSessionTool("todo_create", [](const ToolBuildContext& ctx) {
        return std::make_unique<TodoCreateTool>(ctx.todoList);
    });
    RegisterSessionTool("todo_complete", [](const ToolBuildContext& ctx) {
        return std::make_unique<TodoCompleteTool>(ctx.todoList);
    });
    RegisterSessionTool("todo_insert", [](const ToolBuildContext& ctx) {
        return std::make_unique<TodoInsertTool>(ctx.todoList);
    });
    RegisterSessionTool("todo_remove", [](const ToolBuildContext& ctx) {
        return std::make_unique<TodoRemoveTool>(ctx.todoList);
    });
    RegisterSessionTool("todo_list", [](const ToolBuildContext& ctx) {
        return std::make_unique<TodoListTool>(ctx.todoList);
    });
    RegisterSessionTool("ask_user", [](const ToolBuildContext& ctx) {
        return std::make_unique<AskUserTool>(ctx.askUser, ctx.streamCallback);
    });
    RegisterSessionTool("memory_read_payload", [](const ToolBuildContext& ctx) {
        return std::make_unique<MemoryReadPayloadTool>(ctx.memoryRuntime);
    });
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
    toolSchemaCache_.erase(name);   // structured schema cache invalidated
}

void ResourceManager::RegisterSessionTool(const std::string& name, SessionToolFactory factory)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sessionToolFactories_[name] = std::move(factory);
    sessionToolSchemas_.erase(name);
    toolSchemaCache_.erase(name);   // structured schema cache invalidated
}

std::unique_ptr<Tool> ResourceManager::CreateSessionTool(const std::string& name, const ToolBuildContext& ctx)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessionToolFactories_.find(name);
    if (it == sessionToolFactories_.end()) {
        throw std::runtime_error("Session tool not found: " + name);
    }
    return it->second(ctx);
}

bool ResourceManager::HasSessionTool(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sessionToolFactories_.count(name) > 0;
}

std::vector<std::string> ResourceManager::GetAvailableSessionToolNames() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(sessionToolFactories_.size());
    for (const auto& p : sessionToolFactories_) {
        out.push_back(p.first);
    }
    return out;
}

std::string ResourceManager::GetSessionToolSchema(const std::string& name)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessionToolSchemas_.find(name);
        if (it != sessionToolSchemas_.end()) {
            return it->second;
        }
    }
    // Build a probe instance with an empty context to query the schema. Tool
    // schemas must not depend on ToolBuildContext values.
    ToolBuildContext probeCtx;
    std::unique_ptr<Tool> probe;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessionToolFactories_.find(name);
        if (it == sessionToolFactories_.end()) {
            throw std::runtime_error("Session tool not found: " + name);
        }
        probe = it->second(probeCtx);
    }
    std::string schema = probe->GetSchema();
    std::lock_guard<std::mutex> lock(mutex_);
    sessionToolSchemas_[name] = schema;
    return schema;
}

std::vector<ToolSchema> ResourceManager::BuildToolSchemas(const std::vector<std::string>& toolNames,
                                                           const ToolBuildContext& ctx)
{
    std::vector<ToolSchema> schemas;
    schemas.reserve(toolNames.size());
    // Session tool schemas are independent of ToolBuildContext fields (see
    // GetSessionToolSchema invariant), so a single cache keyed by tool name
    // is sufficient. We still need a probe instance the first time a tool
    // is encountered; afterwards every iteration reuses the cached schema.
    for (const auto& name : toolNames) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto cached = toolSchemaCache_.find(name);
            if (cached != toolSchemaCache_.end()) {
                schemas.push_back(cached->second);
                continue;
            }
        }

        std::unique_ptr<Tool> probe;
        try {
            if (HasSessionTool(name)) {
                probe = CreateSessionTool(name, ctx);
            } else if (HasTool(name)) {
                probe = CreateTool(name);
            } else {
                continue;
            }
        } catch (const std::exception&) {
            continue;
        }
        if (!probe) continue;

        ToolSchema s;
        s.name = probe->GetName();
        s.description = probe->GetDescription();
        s.parameters = probe->GetJsonSchema();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            toolSchemaCache_[name] = s;
        }
        schemas.push_back(std::move(s));
    }
    return schemas;
}

void ResourceManager::RegisterModel(
    ModelFormatType type, std::function<std::unique_ptr<Model>(const ModelConfig&)> factory)
{
    std::lock_guard<std::mutex> lock(mutex_);
    modelFactories_[type] = std::move(factory);
}

void ResourceManager::RegisterModel(
    const std::string& provider, std::function<std::unique_ptr<Model>(const ModelConfig&)> factory)
{
    std::lock_guard<std::mutex> lock(mutex_);
    providerModelFactories_[provider] = std::move(factory);
}

void ResourceManager::RegisterMemoryRuntime(
    const std::string& provider, std::function<std::unique_ptr<MemoryRuntime>(const MemoryConfig&)> factory)
{
    std::lock_guard<std::mutex> lock(mutex_);
    memoryFactories_[provider] = std::move(factory);
}

void ResourceManager::LoadMemoryPlugins(const std::string& pluginDir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (pluginDir.empty() || !fs::is_directory(pluginDir, ec)) {
        LOG(INFO) << "[ResourceManager] Memory plugin dir not found, skipping: " << pluginDir;
        return;
    }

#if defined(_WIN32)
    const std::string ext = ".dll";
#else
    const std::string ext = ".so";
#endif

    using RegisterFn = void (*)(ResourceManager&);
    const char* kEntry = "RegisterMemoryPlugin";

    for (const auto& entry : fs::directory_iterator(pluginDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension().string() != ext) continue;

        const std::string path = entry.path().string();
#if defined(_WIN32)
        HMODULE handle = LoadLibraryA(path.c_str());
        if (!handle) {
            LOG(WARN) << "[ResourceManager] Failed to load memory plugin: " << path;
            continue;
        }
        auto fn = reinterpret_cast<RegisterFn>(GetProcAddress(handle, kEntry));
#else
        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            LOG(WARN) << "[ResourceManager] Failed to load memory plugin: " << path
                      << " (" << (dlerror() ? dlerror() : "unknown") << ")";
            continue;
        }
        auto fn = reinterpret_cast<RegisterFn>(dlsym(handle, kEntry));
#endif
        if (!fn) {
            LOG(WARN) << "[ResourceManager] Memory plugin missing " << kEntry << ": " << path;
            continue;
        }
        fn(*this);
        LOG(INFO) << "[ResourceManager] Loaded memory plugin: " << path;
    }
}

void ResourceManager::RegisterMCPServer(const McpServerConfig& config)
{
    std::shared_ptr<MCPConnection> oldServer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mcpServers_.find(config.id);
        if (it != mcpServers_.end()) {
            oldServer = it->second;
            mcpServers_.erase(it);
        }
    }
    if (oldServer) {
        oldServer->Disconnect();
    }

    MCPEndpointConfig endpointCfg;

    std::string baseUrl = config.url;
    if (baseUrl.empty()) {
        baseUrl = config.endpoint;
    } else if (!config.endpoint.empty()) {
        if (baseUrl.back() != '/' && config.endpoint.front() != '/') {
            baseUrl += "/";
        }
        baseUrl += config.endpoint;
    }
    endpointCfg.url = baseUrl;

    // Determine transport type
    if (!endpointCfg.url.empty()) {
        if (config.type == "sse" ||
            (config.type.empty() && endpointCfg.url.find("/sse") != std::string::npos)) {
            endpointCfg.transportType = MCPTransportType::SSE;
        } else {
            endpointCfg.transportType = MCPTransportType::STREAMABLE_HTTP;
        }
        // Copy headers (std::map -> std::unordered_map)
        for (const auto& kv : config.headers) {
            endpointCfg.headers[kv.first] = kv.second;
        }
    } else if (!config.command.empty()) {
        endpointCfg.transportType = MCPTransportType::STDIO;
        endpointCfg.command = config.command;
        endpointCfg.args = config.args;
        for (const auto& kv : config.env) {
            endpointCfg.env[kv.first] = kv.second;
        }
    } else {
        LOG(ERR) << "Invalid MCP server config: missing 'url' or 'command'";
        return;
    }

    auto server = std::make_shared<MCPConnection>(config.id, endpointCfg);

    // Connect immediately to initialize handshake and discover tools.
    // Connect() catches its own exceptions and leaves the connection in a
    // disconnected state on failure, so an unreachable MCP server cannot
    // prevent the rest of the framework from starting.
    server->Connect();

    // Register after connection attempt so subsequent reconnects or
    // status queries work uniformly for both connected and failed servers.
    std::lock_guard<std::mutex> lock(mutex_);
    mcpServers_[config.id] = server;
}

void ResourceManager::RegisterMcpTool(const std::string& name, std::function<std::unique_ptr<Tool>()> factory)
{
    RegisterTool(name, factory);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mcpToolNames_.insert(name);
    }
}

void ResourceManager::UnregisterMcpTool(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    mcpToolNames_.erase(name);
    toolFactories_.erase(name);
    toolSchemas_.erase(name);
    toolSchemaCache_.erase(name);   // structured schema cache invalidated
}

std::unique_ptr<Tool> ResourceManager::CreateTool(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    LOG(INFO) << "Creating tool instance: " << name;
    auto it = toolFactories_.find(name);
    if (it != toolFactories_.end()) {
        return it->second();
    }
    LOG(INFO) << "Tool not found in factories: " << name;
    throw std::runtime_error("Tool not found: " + name);
}

std::string ResourceManager::GetToolSchema(const std::string& name)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = toolSchemas_.find(name);
        if (it != toolSchemas_.end()) {
            return it->second;
        }
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
        if (it != providerModelFactories_.end()) {
            return it->second(config);
        }
        LOG(WARN) << "[ResourceManager] Provider '" << config.provider
                  << "' not registered; falling back to standard format type "
                  << static_cast<int>(config.formatType);
    }
    
    // 2. Fall back to standard format implementation
    auto it = modelFactories_.find(config.formatType);
    if (it != modelFactories_.end()) {
        return it->second(config);
    }
    throw std::runtime_error("Model format not registered: use ModelFormatType or register custom provider");
}

std::unique_ptr<MemoryRuntime> ResourceManager::CreateMemoryRuntime(const MemoryConfig& config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = memoryFactories_.find(config.provider);
    if (it != memoryFactories_.end()) {
        return it->second(config);
    }
    throw std::runtime_error("Memory runtime provider not registered: " + config.provider);
}

std::shared_ptr<MCPConnection> ResourceManager::GetMCPServer(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = mcpServers_.find(name);
    if (it != mcpServers_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::string> ResourceManager::GetAvailableTools() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(toolFactories_.size() + sessionToolFactories_.size());
    for (const auto& p : toolFactories_) {
        names.push_back(p.first);
    }
    for (const auto& p : sessionToolFactories_) {
        names.push_back(p.first);
    }
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
        if (typeMap.count(p.first)) {
            names.push_back(typeMap[p.first]);
        }
    }
    for (const auto& p : providerModelFactories_) {
        names.push_back(p.first);
    }
    return names;
}

std::vector<std::string> ResourceManager::GetAvailableMemoryRuntimes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(memoryFactories_.size());
    for (const auto& p : memoryFactories_) {
        names.push_back(p.first);
    }
    return names;
}

std::vector<std::string> ResourceManager::GetAvailableMCPServers() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& p : mcpServers_) {
        names.push_back(p.first);
    }
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

bool ResourceManager::HasMemoryRuntime(const std::string& provider) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return memoryFactories_.count(provider) > 0;
}

bool ResourceManager::HasMCPServer(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return mcpServers_.count(name) > 0;
}

void ResourceManager::LoadMCPServers(const std::vector<McpServerConfig>& configs)
{
    MCPConfigManager::Instance().Load(configs);
}

void ResourceManager::UnregisterMCPServer(const std::string& id)
{
    MCPConfigManager::Instance().Remove(id);
}

void ResourceManager::RemoveMCPServerRecord(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    mcpServers_.erase(id);
}

std::vector<McpServerConfig> ResourceManager::GetMCPServerConfigs() const
{
    return MCPConfigManager::Instance().GetAllConfigs();
}

std::vector<std::string> ResourceManager::GetConnectedMCPServerIds() const
{
    return MCPConfigManager::Instance().ActiveIds();
}

std::vector<std::string> ResourceManager::GetMcpToolNames() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(mcpToolNames_.size());
    for (const auto& name : mcpToolNames_) {
        out.push_back(name);
    }
    return out;
}

} // namespace jiuwen

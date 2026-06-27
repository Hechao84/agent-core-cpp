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
    std::lock_guard<std::mutex> lock(toolMutex_);
    toolFactories_[name] = std::move(factory);
    promptSchemas_.erase(name);
    functionCallSchemas_.erase(name);
}

void ResourceManager::RegisterSessionTool(const std::string& name, SessionToolFactory factory)
{
    std::lock_guard<std::mutex> lock(toolMutex_);
    sessionToolFactories_[name] = std::move(factory);
    promptSchemas_.erase(name);
    functionCallSchemas_.erase(name);
}

std::unique_ptr<Tool> ResourceManager::CreateSessionTool(const std::string& name, const ToolBuildContext& ctx)
{
    SessionToolFactory factory;
    {
        std::lock_guard<std::mutex> lock(toolMutex_);
        auto it = sessionToolFactories_.find(name);
        if (it == sessionToolFactories_.end()) {
            throw std::runtime_error("Session tool not found: " + name);
        }
        factory = it->second;
    }
    return factory(ctx);
}

bool ResourceManager::HasSessionTool(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(toolMutex_);
    return sessionToolFactories_.count(name) > 0;
}

std::vector<std::string> ResourceManager::GetAvailableSessionToolNames() const
{
    std::lock_guard<std::mutex> lock(toolMutex_);
    std::vector<std::string> out;
    out.reserve(sessionToolFactories_.size());
    for (const auto& p : sessionToolFactories_) {
        out.push_back(p.first);
    }
    return out;
}

std::string ResourceManager::GetToolSchema(const std::string& name)
{
    {
        std::lock_guard<std::mutex> lock(toolMutex_);
        auto it = promptSchemas_.find(name);
        if (it != promptSchemas_.end()) {
            return it->second;
        }
    }

    // Create a probe instance outside the lock to get the human-readable
    // schema text (used in fallback / prompt-only mode). Works for all
    // tool types (stateless, session-scoped, MCP) — the caller no longer
    // needs to distinguish between HasSessionTool and HasTool.
    std::unique_ptr<Tool> probe;
    {
        std::lock_guard<std::mutex> lock(toolMutex_);
        auto sit = sessionToolFactories_.find(name);
        if (sit != sessionToolFactories_.end()) {
            ToolBuildContext probeCtx;
            probe = sit->second(probeCtx);
        } else {
            auto tit = toolFactories_.find(name);
            if (tit != toolFactories_.end()) {
                probe = tit->second();
            } else {
                throw std::runtime_error("Tool not found: " + name);
            }
        }
    }

    std::string schema = probe->GetSchema();
    std::lock_guard<std::mutex> lock(toolMutex_);
    promptSchemas_[name] = schema;
    return schema;
}

std::vector<ToolSchema> ResourceManager::BuildToolSchemas(const std::vector<std::string>& toolNames,
                                                           const ToolBuildContext& ctx)
{
    std::vector<ToolSchema> schemas;
    schemas.reserve(toolNames.size());

    for (const auto& name : toolNames) {
        ToolSchema cachedSchema;
        bool hasCache = false;
        bool isSession = false;
        bool isStateless = false;
        SessionToolFactory sessionFactory;
        std::function<std::unique_ptr<Tool>()> statelessFactory;

        {
            std::lock_guard<std::mutex> lock(toolMutex_);
            auto cached = functionCallSchemas_.find(name);
            if (cached != functionCallSchemas_.end()) {
                cachedSchema = cached->second;
                hasCache = true;
            } else {
                auto sit = sessionToolFactories_.find(name);
                if (sit != sessionToolFactories_.end()) {
                    isSession = true;
                    sessionFactory = sit->second;
                } else {
                    auto tit = toolFactories_.find(name);
                    if (tit != toolFactories_.end()) {
                        isStateless = true;
                        statelessFactory = tit->second;
                    }
                }
            }
        }

        if (hasCache) {
            schemas.push_back(std::move(cachedSchema));
            continue;
        }
        if (!isSession && !isStateless) {
            continue;
        }

        std::unique_ptr<Tool> probe;
        try {
            if (isSession) {
                probe = sessionFactory(ctx);
            } else {
                probe = statelessFactory();
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
            std::lock_guard<std::mutex> lock(toolMutex_);
            functionCallSchemas_[name] = s;
        }
        schemas.push_back(std::move(s));
    }
    return schemas;
}

void ResourceManager::RegisterModel(
    ModelFormatType type, std::function<std::unique_ptr<Model>(const ModelConfig&)> factory)
{
    std::lock_guard<std::mutex> lock(modelMutex_);
    modelFactories_[type] = std::move(factory);
}

void ResourceManager::RegisterModel(
    const std::string& provider, std::function<std::unique_ptr<Model>(const ModelConfig&)> factory)
{
    std::lock_guard<std::mutex> lock(modelMutex_);
    providerModelFactories_[provider] = std::move(factory);
}

void ResourceManager::RegisterMemoryRuntime(
    const std::string& provider, std::function<std::unique_ptr<MemoryRuntime>(const MemoryConfig&)> factory)
{
    std::lock_guard<std::mutex> lock(memoryMutex_);
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
            // dlopen/LoadLibraryA succeeded but the entry point is absent;
            // close the handle so a bad plugin does not leak.
#if defined(_WIN32)
            FreeLibrary(handle);
#else
            dlclose(handle);
#endif
            continue;
        }

        // Snapshot existing provider names so we can tell which providers this
        // plugin registers. fn(*this) is called OUTSIDE memoryMutex_ because it
        // calls RegisterMemoryRuntime which locks the same mutex (would deadlock).
        std::vector<std::string> before;
        {
            std::lock_guard<std::mutex> lock(memoryMutex_);
            before.reserve(memoryFactories_.size());
            for (const auto& kv : memoryFactories_) before.push_back(kv.first);
        }

        fn(*this);

        {
            std::lock_guard<std::mutex> lock(memoryMutex_);
            for (const auto& kv : memoryFactories_) {
                if (std::find(before.begin(), before.end(), kv.first) == before.end()) {
                    pluginProviders_.push_back(kv.first);
                }
            }
            pluginHandles_.push_back(static_cast<void*>(handle));
        }
        LOG(INFO) << "[ResourceManager] Loaded memory plugin: " << path;
    }
}

void ResourceManager::UnloadMemoryPlugins()
{
    std::lock_guard<std::mutex> lock(memoryMutex_);
    // 1. Erase plugin-registered providers first (their factory lambdas live in
    //    the plugin .so); built-in factories are preserved. This must happen
    //    BEFORE dlclose so no surviving lambda references unloaded code.
    for (const auto& p : pluginProviders_) {
        memoryFactories_.erase(p);
    }
    pluginProviders_.clear();
    // 2. Close the handles.
    for (void* h : pluginHandles_) {
        if (!h) continue;
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(h));
#else
        dlclose(h);
#endif
    }
    pluginHandles_.clear();
}

ResourceManager::~ResourceManager()
{
    UnloadMemoryPlugins();
}

void ResourceManager::RegisterMCPServer(const McpServerConfig& config)
{
    std::shared_ptr<MCPConnection> oldServer;
    {
        std::lock_guard<std::mutex> lock(mcpMutex_);
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

    if (!endpointCfg.url.empty()) {
        if (config.type == "sse" ||
            (config.type.empty() && endpointCfg.url.find("/sse") != std::string::npos)) {
            endpointCfg.transportType = MCPTransportType::SSE;
        } else {
            endpointCfg.transportType = MCPTransportType::STREAMABLE_HTTP;
        }
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

    server->Connect();

    std::lock_guard<std::mutex> lock(mcpMutex_);
    mcpServers_[config.id] = server;
}

void ResourceManager::RegisterMcpTool(const std::string& name, std::function<std::unique_ptr<Tool>()> factory)
{
    RegisterTool(name, factory);
    std::lock_guard<std::mutex> lock(toolMutex_);
    mcpToolNames_.insert(name);
}

void ResourceManager::UnregisterMcpTool(const std::string& name)
{
    std::lock_guard<std::mutex> lock(toolMutex_);
    mcpToolNames_.erase(name);
    toolFactories_.erase(name);
    promptSchemas_.erase(name);
    functionCallSchemas_.erase(name);
}

std::unique_ptr<Tool> ResourceManager::CreateTool(const std::string& name)
{
    std::function<std::unique_ptr<Tool>()> factory;
    {
        std::lock_guard<std::mutex> lock(toolMutex_);
        LOG(INFO) << "Creating tool instance: " << name;
        auto it = toolFactories_.find(name);
        if (it == toolFactories_.end()) {
            LOG(INFO) << "Tool not found in factories: " << name;
            throw std::runtime_error("Tool not found: " + name);
        }
        factory = it->second;
    }
    return factory();
}

std::unique_ptr<Model> ResourceManager::CreateModel(const ModelConfig& config)
{
    std::function<std::unique_ptr<Model>(const ModelConfig&)> factory;
    {
        std::lock_guard<std::mutex> lock(modelMutex_);

        if (!config.provider.empty()) {
            auto it = providerModelFactories_.find(config.provider);
            if (it != providerModelFactories_.end()) {
                factory = it->second;
            } else {
                LOG(WARN) << "[ResourceManager] Provider '" << config.provider
                           << "' not registered; falling back to standard format type "
                           << static_cast<int>(config.formatType);
            }
        }

        if (!factory) {
            auto it = modelFactories_.find(config.formatType);
            if (it != modelFactories_.end()) {
                factory = it->second;
            }
        }

        if (!factory) {
            throw std::runtime_error("Model format not registered: use ModelFormatType or register custom provider");
        }
    }
    return factory(config);
}

std::unique_ptr<MemoryRuntime> ResourceManager::CreateMemoryRuntime(const MemoryConfig& config)
{
    std::function<std::unique_ptr<MemoryRuntime>(const MemoryConfig&)> factory;
    {
        std::lock_guard<std::mutex> lock(memoryMutex_);
        auto it = memoryFactories_.find(config.provider);
        if (it == memoryFactories_.end()) {
            throw std::runtime_error("Memory runtime provider not registered: " + config.provider);
        }
        factory = it->second;
    }
    return factory(config);
}

std::shared_ptr<MCPConnection> ResourceManager::GetMCPServer(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mcpMutex_);
    auto it = mcpServers_.find(name);
    if (it != mcpServers_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::string> ResourceManager::GetAvailableTools() const
{
    std::lock_guard<std::mutex> lock(toolMutex_);
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
    std::lock_guard<std::mutex> lock(modelMutex_);
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
    std::lock_guard<std::mutex> lock(memoryMutex_);
    std::vector<std::string> names;
    names.reserve(memoryFactories_.size());
    for (const auto& p : memoryFactories_) {
        names.push_back(p.first);
    }
    return names;
}

std::vector<std::string> ResourceManager::GetAvailableMCPServers() const
{
    std::lock_guard<std::mutex> lock(mcpMutex_);
    std::vector<std::string> names;
    for (const auto& p : mcpServers_) {
        names.push_back(p.first);
    }
    return names;
}

bool ResourceManager::HasTool(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(toolMutex_);
    return toolFactories_.count(name) > 0;
}

bool ResourceManager::HasModel(ModelFormatType type) const
{
    std::lock_guard<std::mutex> lock(modelMutex_);
    return modelFactories_.count(type) > 0;
}

bool ResourceManager::HasModel(const std::string& provider) const
{
    std::lock_guard<std::mutex> lock(modelMutex_);
    return providerModelFactories_.count(provider) > 0;
}

bool ResourceManager::HasMemoryRuntime(const std::string& provider) const
{
    std::lock_guard<std::mutex> lock(memoryMutex_);
    return memoryFactories_.count(provider) > 0;
}

bool ResourceManager::HasMCPServer(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mcpMutex_);
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
    std::lock_guard<std::mutex> lock(mcpMutex_);
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
    std::lock_guard<std::mutex> lock(toolMutex_);
    std::vector<std::string> out;
    out.reserve(mcpToolNames_.size());
    for (const auto& name : mcpToolNames_) {
        out.push_back(name);
    }
    return out;
}

} // namespace jiuwen

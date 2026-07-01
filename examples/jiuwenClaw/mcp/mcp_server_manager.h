#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "include/types.h"

namespace jiuwenClaw {

// Application-layer MCP server entry (persisted to mcp_servers.json).
// Mirrors jiuwen::McpServerConfig in shape but has its own type to keep
// application-level fields (e.g. UI ordering, tags) decoupled from the
// framework's runtime input struct.
struct McpServerEntry
{
    std::string id;
    std::string name;
    std::string description;
    bool enabled{true};
    std::string type;  // "streamable-http-client" / "sse" / "stdio"
    std::string url;
    std::string endpoint;
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::map<std::string, std::string> headers;
    // HTTP timeouts in seconds (Streamable HTTP / SSE). 0 = curl defaults.
    int connectTimeoutSeconds{3};
    int requestTimeoutSeconds{10};
};

// McpServerManager: application-layer persistent registry for MCP servers.
//
// Responsibilities:
//   - CRUD operations on McpServerEntry list
//   - Persist to / load from mcp_servers.json
//   - Convert entries to jiuwen::McpServerConfig and hand them to the
//     framework via ResourceManager::LoadMCPServers / RegisterMCPServer.
//
// The framework itself is unaware of mcp_servers.json; other agent
// applications can either use this class directly, write their own loader,
// or just hard-code McpServerConfig instances at startup.
class McpServerManager
{
public:
    static McpServerManager& GetInstance();

    void SetPersistPath(const std::string& path);
    bool Load();
    bool Save();

    void AddServer(const McpServerEntry& entry);
    void UpdateServer(const std::string& id, const McpServerEntry& entry);
    void RemoveServer(const std::string& id);
    McpServerEntry* GetServer(const std::string& id);
    std::vector<McpServerEntry> GetAllServers() const;

    // Convert all enabled entries to framework-layer McpServerConfig.
    // Used at startup to bulk-register with ResourceManager::LoadMCPServers.
    std::vector<jiuwen::McpServerConfig> ToFrameworkConfigs() const;

    // Convert a single entry (by id) to framework McpServerConfig.
    // Returns true if found and converted.
    bool ToFrameworkConfig(const std::string& id, jiuwen::McpServerConfig& out) const;

private:
    McpServerManager() = default;
    void SaveToFile();
    static jiuwen::McpServerConfig EntryToConfig(const McpServerEntry& e);

    std::string persistPath_;
    mutable std::mutex mutex_;
    std::map<std::string, McpServerEntry> servers_;
};

} // namespace jiuwenClaw

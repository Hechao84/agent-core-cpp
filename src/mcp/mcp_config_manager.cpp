#include "src/mcp/mcp_config_manager.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "include/resource_manager.h"
#include "src/mcp/mcp_connection.h"
#include "src/utils/logger.h"

namespace jiuwen {

MCPConfigManager& MCPConfigManager::Instance()
{
    static MCPConfigManager instance;
    return instance;
}

void MCPConfigManager::Load(const std::vector<McpServerConfig>& configs)
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> toRemove;
    for (const auto& kv : lastConfigs_) {
        bool found = false;
        for (const auto& config : configs) {
            if (config.id == kv.first) {
                found = true;
                break;
            }
        }
        if (!found) {
            toRemove.push_back(kv.first);
        }
    }

    for (const auto& id : toRemove) {
        StopServerLocked(id);
    }

    for (const auto& config : configs) {
        Apply(config);
    }
}

void MCPConfigManager::Apply(const McpServerConfig& config)
{
    auto it = lastConfigs_.find(config.id);
    bool exists = (it != lastConfigs_.end());

    if (!config.enabled) {
        if (exists) {
            StopServerLocked(config.id);
        }
        lastConfigs_[config.id] = config;
        return;
    }

    if (exists && !ConfigChanged(it->second, config)) {
        return;
    }

    StopServerLocked(config.id);
    StartServerLocked(config);
    lastConfigs_[config.id] = config;
}

void MCPConfigManager::Remove(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    StopServerLocked(id);
    lastConfigs_.erase(id);
}

void MCPConfigManager::StopAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : servers_) {
        kv.second->Disconnect();
    }
    servers_.clear();
    lastConfigs_.clear();
}

std::vector<McpServerConfig> MCPConfigManager::GetAllConfigs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<McpServerConfig> configs;
    for (const auto& kv : lastConfigs_) {
        configs.push_back(kv.second);
    }
    return configs;
}

std::vector<std::string> MCPConfigManager::ActiveIds() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    for (const auto& kv : servers_) {
        if (kv.second && kv.second->IsConnected()) {
            ids.push_back(kv.first);
        }
    }
    return ids;
}

bool MCPConfigManager::ConfigChanged(const McpServerConfig& lhs, const McpServerConfig& rhs) const
{
    if (lhs.name != rhs.name) {
        return true;
    }
    if (lhs.description != rhs.description) {
        return true;
    }
    if (lhs.enabled != rhs.enabled) {
        return true;
    }
    if (lhs.type != rhs.type) {
        return true;
    }
    if (lhs.url != rhs.url) {
        return true;
    }
    if (lhs.endpoint != rhs.endpoint) {
        return true;
    }
    if (lhs.command != rhs.command) {
        return true;
    }
    if (lhs.args != rhs.args) {
        return true;
    }
    if (lhs.env != rhs.env) {
        return true;
    }
    return lhs.headers != rhs.headers;
}

void MCPConfigManager::StartServerLocked(const McpServerConfig& config)
{
    ResourceManager::GetInstance().RegisterMCPServer(config);
    auto server = ResourceManager::GetInstance().GetMCPServer(config.id);
    if (server) {
        servers_[config.id] = server;
    }
}

void MCPConfigManager::StopServerLocked(const std::string& id)
{
    auto it = servers_.find(id);
    if (it != servers_.end()) {
        if (it->second) {
            it->second->Disconnect();
        }
        servers_.erase(it);
        ResourceManager::GetInstance().RemoveMCPServerRecord(id);
    }
}

} // namespace jiuwen

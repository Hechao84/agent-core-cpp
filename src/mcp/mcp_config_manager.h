#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "include/agent_export.h"
#include "include/types.h"
#include "src/mcp/mcp_connection.h"

namespace jiuwen {

class AGENT_API MCPConfigManager
{
public:
    static MCPConfigManager& Instance();

    void Load(const std::vector<McpServerConfig>& configs);
    void Apply(const McpServerConfig& config);
    void Remove(const std::string& id);
    void StopAll();
    std::vector<McpServerConfig> GetAllConfigs() const;
    std::vector<std::string> ActiveIds() const;

private:
    MCPConfigManager() = default;

    bool ConfigChanged(const McpServerConfig& lhs, const McpServerConfig& rhs) const;
    void StartServerLocked(const McpServerConfig& config);
    void StopServerLocked(const std::string& id);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, McpServerConfig> lastConfigs_;
    std::unordered_map<std::string, std::shared_ptr<MCPConnection>> servers_;
};

} // namespace jiuwen

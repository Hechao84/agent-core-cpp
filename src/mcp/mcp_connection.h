#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "include/tool.h"
#include "src/mcp/mcp_client.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

enum class MCPTransportType
{
    STDIO,
    SSE,
    STREAMABLE_HTTP
};

struct MCPEndpointConfig
{
    std::string command;
    std::vector<std::string> args;
    std::string url;
    MCPTransportType transportType{MCPTransportType::STREAMABLE_HTTP};
    std::unordered_map<std::string, std::string> env;
    std::unordered_map<std::string, std::string> headers;
};

class MCPTool;

class MCPConnection : public std::enable_shared_from_this<MCPConnection>
{
public:
    MCPConnection(std::string name, MCPEndpointConfig config);
    void Connect();
    void Disconnect();
    std::vector<std::string> ListTools();
    std::shared_ptr<MCPTool> GetTool(const std::string& toolName);
    std::shared_ptr<MCPToolResult> CallTool(const std::string& toolName, const std::string& arguments);
    bool IsConnected() const;
    std::string GetName() const;

private:
    std::string name_;
    MCPEndpointConfig config_;
    mutable std::mutex stateMutex_;  // Lock layer L6 (connection state)
    bool connected_{false};
    std::vector<MCPToolInfo> availableTools_;
    std::shared_ptr<MCPClient> client_;
    mutable std::mutex callMutex_;  // Lock layer L7 (call serialization)

    void CreateClient();
};

} // namespace jiuwen

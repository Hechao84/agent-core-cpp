#pragma once
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "tool.h"

// SDK headers
#include <mcp/mcp_client.h>
#include <mcp/mcp_type.h>

enum class MCPTransportType
{
    STDIO,
    HTTP
};

struct MCPEndpointConfig
{
    std::string command;
    std::vector<std::string> args;
    std::string url;
    MCPTransportType transportType{MCPTransportType::STDIO};
    std::unordered_map<std::string, std::string> env;
    std::unordered_map<std::string, std::string> headers;
};

class MCPTool; // Forward declaration

class MCPServer : public std::enable_shared_from_this<MCPServer>
{
public:
    MCPServer(std::string name, MCPEndpointConfig config);
    void Connect();
    void Disconnect();
    std::vector<std::string> ListTools();
    std::shared_ptr<MCPTool> GetTool(const std::string& toolName);
    std::shared_ptr<Mcp::CallToolResult> CallTool(const std::string& toolName, const std::string& arguments);
    bool IsConnected() const;
    std::string GetName() const;

private:
    std::string name_;
    MCPEndpointConfig config_;
    bool connected_{false};
    std::vector<Mcp::Tool> availableTools_;
    std::shared_ptr<Mcp::McpClient> client_;
    void CreateClient();
};

class MCPTool : public Tool
{
public:
    MCPTool(std::string name, std::string description, std::vector<ToolParam> params, std::shared_ptr<MCPServer> server);
    std::string Invoke(const std::string& input) override;

private:
    std::shared_ptr<MCPServer> server_;
};

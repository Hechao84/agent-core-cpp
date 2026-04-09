#include "tools/mcp_tool.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <memory>

class MCPServerImpl {
    public:
        std::vector<std::string> FetchToolsList() {
            std::cout << "[MCPServer] Fetching tools..." << std::endl;
            return {"mcp_tool_1", "mcp_tool_2"};
        }
};

MCPTool::MCPTool(std::string name, std::string description, std::vector<ToolParam> params, std::weak_ptr<MCPServer> server)
    : Tool(std::move(name), std::move(description), std::move(params)), m_server(std::move(server)) {}

std::string MCPTool::Invoke(const std::string& input) {
    auto serverPtr = m_server.lock();
    if (!serverPtr || !serverPtr->IsConnected()) {
        return "Error: MCP Server not connected";
    }
    return "MCP Response: " + input;
}

MCPServer::MCPServer(std::string name, MCPEndpointConfig config) : m_name(std::move(name)), m_config(std::move(config)) {}

void MCPServer::Connect() {
    std::cout << "[MCPServer] Connecting: " << m_name << std::endl;
    m_connected = true;
    m_availableTools = FetchToolsList();
}

void MCPServer::Disconnect() {
    std::cout << "[MCPServer] Disconnecting: " << m_name << std::endl;
    m_connected = false;
    m_availableTools.clear();
}

std::vector<std::string> MCPServer::ListTools() {
    if (!m_connected) Connect();
    return m_availableTools;
}

std::unique_ptr<MCPTool> MCPServer::GetTool(const std::string& toolName) {
    if (!m_connected) Connect();
    if (std::find(m_availableTools.begin(), m_availableTools.end(), toolName) == m_availableTools.end()) return nullptr;
    std::vector<ToolParam> params = {{"input", "Input for the tool", "string", true}};
    return std::make_unique<MCPTool>(toolName, "MCP Tool: " + toolName, params, shared_from_this());
}

bool MCPServer::IsConnected() const { return m_connected; }
std::string MCPServer::GetName() const { return m_name; }

std::vector<std::string> MCPServer::FetchToolsList() {
    std::cout << "[MCPServer] Fetching tools..." << std::endl;
    return {"mcp_tool_1", "mcp_tool_2"};
}

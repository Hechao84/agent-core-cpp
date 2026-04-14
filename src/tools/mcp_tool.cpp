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
    : Tool(std::move(name), std::move(description), std::move(params)), server_(std::move(server)) {}

std::string MCPTool::Invoke(const std::string& input) {
    auto serverPtr = server_.lock();
    if (!serverPtr || !serverPtr->IsConnected()) {
        return "Error: MCP Server not connected";
    }
    return "MCP Response: " + input;
}

MCPServer::MCPServer(std::string name, MCPEndpointConfig config) : name_(std::move(name)), config_(std::move(config)) {}

void MCPServer::Connect() {
    std::cout << "[MCPServer] Connecting: " << name_ << std::endl;
    connected_ = true;
    availableTools_ = FetchToolsList();
}

void MCPServer::Disconnect() {
    std::cout << "[MCPServer] Disconnecting: " << name_ << std::endl;
    connected_ = false;
    availableTools_.clear();
}

std::vector<std::string> MCPServer::ListTools() {
    if (!connected_) Connect();
    return availableTools_;
}

std::unique_ptr<MCPTool> MCPServer::GetTool(const std::string& toolName) {
    if (!connected_) Connect();
    if (std::find(availableTools_.begin(), availableTools_.end(), toolName) == availableTools_.end()) return nullptr;
    std::vector<ToolParam> params = {{"input", "Input for the tool", "string", true}};
    return std::make_unique<MCPTool>(toolName, "MCP Tool: " + toolName, params, shared_from_this());
}

bool MCPServer::IsConnected() const { return connected_; }
std::string MCPServer::GetName() const { return name_; }

std::vector<std::string> MCPServer::FetchToolsList() {
    std::cout << "[MCPServer] Fetching tools..." << std::endl;
    return {"mcp_tool_1", "mcp_tool_2"};
}

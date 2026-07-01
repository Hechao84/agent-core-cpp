#include "src/mcp/mcp_connection.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "include/resource_manager.h"
#include "include/tool.h"
#include "src/mcp/mcp_client.h"
#include "src/mcp/mcp_tool.h"
#include "src/utils/logger.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

MCPConnection::MCPConnection(std::string name, MCPEndpointConfig config)
    : name_(std::move(name)), config_(std::move(config))
{
}

void MCPConnection::CreateClient()
{
    if (config_.transportType == MCPTransportType::SSE ||
        config_.transportType == MCPTransportType::STREAMABLE_HTTP) {
        client_ = std::make_shared<MCPClient>(name_, "1.0.0", config_.url,
                                              config_.connectTimeoutSeconds,
                                              config_.requestTimeoutSeconds);
    } else {
        std::cerr << "Warning: Only Streamable HTTP/SSE is supported by the custom MCP client" << std::endl;
    }
}

void MCPConnection::Connect()
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (connected_) {
        return;
    }

    std::cout << "[MCPConnection] Connecting: " << name_ << std::endl;
    CreateClient();

    if (!client_) {
        std::cerr << "[MCPConnection] Failed to create client" << std::endl;
        connected_ = false;
        return;
    }

    if (!client_->Initialize()) {
        std::cerr << "[MCPConnection] Failed to connect: " << client_->GetLastError() << std::endl;
        connected_ = false;
        return;
    }

    std::vector<MCPToolInfo> tools = client_->ListTools();
    if (!client_->GetLastError().empty()) {
        std::cerr << "[MCPConnection] Failed to list tools: " << client_->GetLastError() << std::endl;
    }

    availableTools_.clear();
    for (const auto& toolDef : tools) {
        availableTools_.push_back(toolDef);
        std::cout << "[MCPConnection] Discovered tool: " << toolDef.name << std::endl;
        LOG(INFO) << "MCP Server [" << name_ << "] discovered tool: " << toolDef.name;

        std::string toolName = toolDef.name;
        std::string toolDescription = toolDef.description;
        if (toolDescription.empty()) {
            toolDescription = "MCP Tool: " + toolName;
        }
        auto serverPtr = shared_from_this();
        auto& resourceManager = ResourceManager::GetInstance();

        std::vector<ToolParam> params;
        bool hasSchema = false;
        if (toolDef.inputSchema.contains("properties") && toolDef.inputSchema["properties"].is_object()) {
            const auto& properties = toolDef.inputSchema["properties"];
            for (auto it = properties.begin(); it != properties.end(); ++it) {
                ToolParam param;
                param.name = it.key();
                param.description = it->value("description", "Parameter " + param.name);
                param.type = it->value("type", "string");
                param.required = false;

                if (toolDef.inputSchema.contains("required") && toolDef.inputSchema["required"].is_array()) {
                    for (const auto& requiredName : toolDef.inputSchema["required"]) {
                        if (requiredName.is_string() && requiredName.get<std::string>() == param.name) {
                            param.required = true;
                            break;
                        }
                    }
                }
                params.push_back(param);
                hasSchema = true;
            }
        }

        if (!hasSchema) {
            params = {{"input", "JSON input arguments", "string", true}};
        }

        resourceManager.RegisterMcpTool(toolName, [toolName, toolDescription, serverPtr, params]() {
            return std::make_unique<MCPTool>(toolName, toolDescription, params, serverPtr);
        });
    }

    connected_ = true;
    std::cout << "[MCPConnection] Connection established successfully." << std::endl;
}

void MCPConnection::Disconnect()
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::cout << "[MCPConnection] Disconnecting: " << name_ << std::endl;

    auto& resourceManager = ResourceManager::GetInstance();
    for (const auto& tool : availableTools_) {
        resourceManager.UnregisterMcpTool(tool.name);
    }

    connected_ = false;
    availableTools_.clear();
    client_.reset();
}

std::vector<std::string> MCPConnection::ListTools()
{
    bool isConnected = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        isConnected = connected_;
    }
    if (!isConnected) {
        Connect();
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    std::vector<std::string> names;
    for (const auto& tool : availableTools_) {
        names.push_back(tool.name);
    }
    return names;
}

std::shared_ptr<MCPTool> MCPConnection::GetTool(const std::string& toolName)
{
    bool isConnected = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        isConnected = connected_;
    }
    if (!isConnected) {
        Connect();
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    auto it = std::find_if(availableTools_.begin(), availableTools_.end(),
                           [&toolName](const MCPToolInfo& tool) {
                               return tool.name == toolName;
                           });
    if (it == availableTools_.end()) {
        return nullptr;
    }

    std::vector<ToolParam> params = {{"input", "JSON input arguments", "string", true}};
    return std::make_shared<MCPTool>(toolName, "MCP Tool: " + toolName, params, shared_from_this());
}

std::shared_ptr<MCPToolResult> MCPConnection::CallTool(const std::string& toolName, const std::string& arguments)
{
    bool needConnect = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        needConnect = (!client_ || !connected_);
    }
    if (needConnect) {
        Connect();
    }

    std::shared_ptr<MCPClient> clientSnapshot;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        clientSnapshot = client_;
    }

    if (!clientSnapshot) {
        auto result = std::make_shared<MCPToolResult>();
        result->isError = true;
        result->content.push_back("MCP Client not initialized");
        return result;
    }

    nlohmann::json argsJson = nlohmann::json::parse(arguments, nullptr, false);
    if (argsJson.is_discarded()) {
        argsJson = arguments;
    }

    std::lock_guard<std::mutex> callLock(callMutex_);
    return clientSnapshot->CallTool(toolName, argsJson);
}

bool MCPConnection::IsConnected() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return connected_;
}

std::string MCPConnection::GetName() const
{
    return name_;
}

} // namespace jiuwen

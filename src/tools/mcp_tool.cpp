#include "tools/mcp_tool.h"
#include "../protocol/mcp_client.h"
#include "resource_manager.h"

#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>

#include <algorithm>

MCPTool::MCPTool(std::string name, std::string description, std::vector<ToolParam> params, std::shared_ptr<MCPServer> server)
    : Tool(std::move(name), std::move(description), std::move(params)), server_(std::move(server))
{
}

std::string MCPTool::Invoke(const std::string& input)
{
    if (!server_ || !server_->IsConnected())
    {
        return "Error: MCP Server not connected or invalid";
    }

    try
    {
        nlohmann::json args = nlohmann::json::parse(input);
        std::shared_ptr<MCPToolResult> result = server_->CallTool(name_, args);

        if (!result)
        {
            return "Error: MCP tool call returned null result";
        }

        if (result->isError)
        {
            std::string out = "MCP Tool Error: ";
            for (const auto& c : result->content)
            {
                out += c;
            }
            return out;
        }

        std::string out;
        for (const auto& c : result->content)
        {
            out += c;
        }
        return out;
    }
    catch (const std::exception& e)
    {
        return "Error: MCP tool call failed: " + std::string(e.what());
    }
}

MCPServer::MCPServer(std::string name, MCPEndpointConfig config)
    : name_(std::move(name)), config_(std::move(config))
{
}

void MCPServer::CreateClient()
{
    if (config_.transportType == MCPTransportType::SSE ||
        config_.transportType == MCPTransportType::STREAMABLE_HTTP)
    {
        client_ = std::make_shared<MCPClient>(name_, "1.0.0", config_.url);

        if (!config_.headers.empty())
        {
            // Custom headers handling is currently not implemented in MCPClient.
            // It is reserved for future use.
        }
    }
    else
    {
        std::cerr << "Warning: Only Streamable HTTP/SSE is supported by the custom MCP client" << std::endl;
    }
}

void MCPServer::Connect()
{
    if (connected_)
    {
        return;
    }

    std::cout << "[MCPServer] Connecting: " << name_ << std::endl;
    CreateClient();

    if (!client_)
    {
        std::cerr << "[MCPServer] Failed to create client" << std::endl;
        connected_ = false;
        return;
    }

    try
    {
        client_->Initialize();

        std::vector<MCPToolInfo> tools = client_->ListTools();

        availableTools_.clear();
        for (const auto& toolDef : tools)
        {
            availableTools_.push_back(toolDef);
            std::cout << "[MCPServer] Discovered tool: " << toolDef.name << std::endl;

            auto& rm = ResourceManager::GetInstance();
            std::string tName = toolDef.name;
            auto serverPtr = shared_from_this();
            std::vector<ToolParam> params = {{"input", "JSON input arguments", "string", true}};

            rm.RegisterTool(tName, [tName, serverPtr, params]()
            {
                return std::make_unique<MCPTool>(tName, "MCP Tool: " + tName, params, serverPtr);
            });
        }

        connected_ = true;
        std::cout << "[MCPServer] Connection established successfully." << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MCPServer] Failed to connect: " << e.what() << std::endl;
        connected_ = false;
    }
}

void MCPServer::Disconnect()
{
    std::cout << "[MCPServer] Disconnecting: " << name_ << std::endl;
    connected_ = false;
    availableTools_.clear();
    client_.reset();
}

std::vector<std::string> MCPServer::ListTools()
{
    if (!connected_)
    {
        Connect();
    }
    std::vector<std::string> names;
    for (const auto& t : availableTools_)
    {
        names.push_back(t.name);
    }
    return names;
}

std::shared_ptr<MCPTool> MCPServer::GetTool(const std::string& toolName)
{
    if (!connected_)
    {
        Connect();
    }
    auto it = std::find_if(availableTools_.begin(), availableTools_.end(),
                           [&toolName](const MCPToolInfo& t)
                           {
                               return t.name == toolName;
                           });

    if (it == availableTools_.end())
    {
        return nullptr;
    }

    std::vector<ToolParam> params = {{"input", "JSON input arguments", "string", true}};
    return std::make_shared<MCPTool>(toolName, "MCP Tool: " + toolName, params, shared_from_this());
}

std::shared_ptr<MCPToolResult> MCPServer::CallTool(const std::string& toolName, const std::string& arguments)
{
    if (!client_ || !connected_)
    {
        Connect();
    }

    if (!client_)
    {
        throw std::runtime_error("MCP Client not initialized");
    }

    nlohmann::json argsJson;
    try
    {
        argsJson = nlohmann::json::parse(arguments);
    }
    catch (...)
    {
        argsJson = arguments;
    }

    return client_->CallTool(toolName, argsJson);
}

bool MCPServer::IsConnected() const
{
    return connected_;
}

std::string MCPServer::GetName() const
{
    return name_;
}

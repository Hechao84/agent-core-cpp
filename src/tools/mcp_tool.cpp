#include "tools/mcp_tool.h"
#include "resource_manager.h"

// MCP SDK Headers
#include <mcp/mcp_client.h>
#include <mcp/mcp_type.h>

#include <algorithm>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <variant>

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
        // Convert input JSON string to nlohmann::json object for the SDK
        nlohmann::json args = nlohmann::json::parse(input);
        Mcp::JsonValue sdkArgs(args);

        // Call the tool via the server
        std::shared_ptr<Mcp::CallToolResult> result = server_->CallTool(name_, input);

        if (!result)
        {
            return "Error: MCP tool call returned null result";
        }

        // Format result content
        std::ostringstream oss;
        if (result->isError)
        {
            oss << "MCP Tool Error: ";
        }

        for (const auto& content : result->content)
        {
            if (std::holds_alternative<Mcp::TextContent>(content))
            {
                oss << std::get<Mcp::TextContent>(content).text;
            }
            else if (std::holds_alternative<Mcp::ImageContent>(content))
            {
                oss << "[Image: " << std::get<Mcp::ImageContent>(content).mimeType << "]";
            }
            else if (std::holds_alternative<Mcp::AudioContent>(content))
            {
                oss << "[Audio: " << std::get<Mcp::AudioContent>(content).mimeType << "]";
            }
            // Other types like EmbeddedResource or ResourceLink could be handled here
        }

        return oss.str();
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
    Mcp::ClientConfig clientCfg;
    clientCfg.name = name_;

    if (config_.transportType == MCPTransportType::HTTP)
    {
        Mcp::StreamableHttpClientConfig transportCfg;
        transportCfg.endpoint = config_.url;
        if (!config_.headers.empty())
        {
            for (const auto& pair : config_.headers)
            {
                transportCfg.headers[pair.first] = pair.second;
            }
        }

        client_ = Mcp::McpClientFactory::CreateStreamableHttpClient(clientCfg, transportCfg);
    }
    else
    {
        Mcp::StdioClientConfig transportCfg;
        transportCfg.command = config_.command;
        transportCfg.args = config_.args;
        for (const auto& pair : config_.env)
        {
            transportCfg.env[pair.first] = pair.second;
        }

        client_ = Mcp::McpClientFactory::CreateStdioClient(clientCfg, transportCfg);
    }
}

void MCPServer::Connect()
{
    if (connected_) return;

    std::cout << "[MCPServer] Connecting: " << name_ << std::endl;
    CreateClient();

    try
    {
        // Perform initialization handshake
        auto initFuture = client_->Initialize();
        auto initResult = initFuture.get();

        std::cout << "[MCPServer] Initialized with protocol version: " << initResult->protocolVersion << std::endl;

        // List tools
        auto toolsFuture = client_->ListTools();
        auto toolsResult = toolsFuture.get();

        availableTools_.clear();
        for (const auto& toolDef : toolsResult->tools)
        {
            availableTools_.push_back(toolDef);
            std::cout << "[MCPServer] Discovered tool: " << toolDef.name << std::endl;

            // Register tool dynamically into ResourceManager
            auto& rm = ResourceManager::GetInstance();
            std::string tName = toolDef.name;
            auto serverPtr = shared_from_this();
            std::vector<ToolParam> params = {{"input", "JSON input arguments", "string", true}};

            // We use a lambda that creates an MCPTool instance for this specific tool name
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
                           [&toolName](const Mcp::Tool& t)
                           {
                               return t.name == toolName;
                           });

    if (it == availableTools_.end()) return nullptr;

    std::vector<ToolParam> params = {{"input", "JSON input arguments", "string", true}};
    return std::make_shared<MCPTool>(toolName, "MCP Tool: " + toolName, params, shared_from_this());
}

std::shared_ptr<Mcp::CallToolResult> MCPServer::CallTool(const std::string& toolName, const std::string& arguments)
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

    auto callFuture = client_->CallTool(toolName, Mcp::JsonValue(argsJson));
    return callFuture.get();
}

bool MCPServer::IsConnected() const
{
    return connected_;
}
std::string MCPServer::GetName() const
{
    return name_;
}

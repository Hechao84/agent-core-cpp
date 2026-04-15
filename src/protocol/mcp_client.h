#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

struct MCPToolInfo
{
    std::string name;
    std::string description;
    nlohmann::json inputSchema;
};

struct MCPToolResult
{
    bool isError{false};
    std::vector<std::string> content;
};

class MCPClient
{
public:
    MCPClient(const std::string& name, const std::string& version, const std::string& endpoint);
    ~MCPClient();

    void Initialize();
    std::vector<MCPToolInfo> ListTools();
    std::shared_ptr<MCPToolResult> CallTool(const std::string& toolName, const nlohmann::json& arguments);

    const std::string& GetEndpoint() const { return endpoint_; }
    bool IsInitialized() const { return isInitialized_; }

private:
    nlohmann::json SendRequest(const nlohmann::json& request);

    std::string name_;
    std::string version_;
    std::string endpoint_;
    std::string sessionId_;
    bool isInitialized_{false};
    std::vector<std::string> headers_;
    int nextRequestId_{1};
};

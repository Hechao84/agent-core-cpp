#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

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
    // connectTimeoutSeconds / requestTimeoutSeconds: 0 = leave curl defaults.
    // Defaults preserve the previous hardcoded 3s/10s behavior.
    MCPClient(const std::string& name, const std::string& version, const std::string& endpoint,
              long connectTimeoutSeconds = 3, long requestTimeoutSeconds = 10);
    ~MCPClient();

    bool Initialize();
    std::vector<MCPToolInfo> ListTools();
    std::shared_ptr<MCPToolResult> CallTool(const std::string& toolName, const nlohmann::json& arguments);

    const std::string& GetEndpoint() const { return endpoint_; }
    bool IsInitialized() const { return isInitialized_; }
    const std::string& GetLastError() const { return lastError_; }

private:
    nlohmann::json SendRequest(const nlohmann::json& request);
    nlohmann::json MakeErrorResponse(const std::string& message);

    std::string name_;
    std::string version_;
    std::string endpoint_;
    long connectTimeoutSeconds_{3};
    long requestTimeoutSeconds_{10};
    mutable std::mutex sessionMutex_;  // Lock layer L7 (MCP client session ID)
    std::string sessionId_;
    bool isInitialized_{false};
    std::vector<std::string> headers_;
    std::atomic<int> nextRequestId_{1};
    std::string lastError_;
};

} // namespace jiuwen

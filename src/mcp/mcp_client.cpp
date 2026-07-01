#include "src/mcp/mcp_client.h"

#include <memory>
#include <string>
#include <vector>

#include "src/utils/curl_client.h"
#include "src/utils/logger.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

MCPClient::MCPClient(const std::string& name, const std::string& version, const std::string& endpoint,
                     long connectTimeoutSeconds, long requestTimeoutSeconds)
    : name_(name), version_(version), endpoint_(endpoint),
      connectTimeoutSeconds_(connectTimeoutSeconds),
      requestTimeoutSeconds_(requestTimeoutSeconds)
{
    if (endpoint_.empty()) {
        lastError_ = "MCP endpoint must not be empty";
    }
}

MCPClient::~MCPClient() = default;

nlohmann::json MCPClient::MakeErrorResponse(const std::string& message)
{
    lastError_ = message;
    nlohmann::json response;
    response["error"]["message"] = message;
    return response;
}

nlohmann::json MCPClient::SendRequest(const nlohmann::json& request)
{
    if (endpoint_.empty()) {
        return MakeErrorResponse("MCP endpoint must not be empty");
    }

    CurlRequest req;
    req.url = endpoint_;
    req.body = request.dump();
    req.headers = {
        "Content-Type: application/json",
        "Accept: application/json, text/event-stream"};
    if (connectTimeoutSeconds_ > 0) {
        req.connectTimeout = connectTimeoutSeconds_;
    }
    if (requestTimeoutSeconds_ > 0) {
        req.requestTimeout = requestTimeoutSeconds_;
    }

    std::string sessionSnapshot;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        sessionSnapshot = sessionId_;
    }
    if (!sessionSnapshot.empty()) {
        req.headers.push_back("Mcp-Session-Id: " + sessionSnapshot);
    }
    for (const auto& header : headers_) {
        req.headers.push_back(header);
    }

    CurlResponse resp = CurlClient::Post(req);

    if (resp.isCurlError) {
        return MakeErrorResponse("CURL error: " + resp.curlErrorStr);
    }

    if (resp.statusCode != 200 && resp.statusCode != 202) {
        std::string error = "HTTP error " + std::to_string(resp.statusCode);
        if (!resp.body.empty()) {
            error += ": " + resp.body;
        }
        return MakeErrorResponse(error);
    }

    if (resp.body.empty()) {
        return nlohmann::json();
    }

    nlohmann::json response = nlohmann::json::parse(resp.body, nullptr, false);
    if (response.is_discarded()) {
        return MakeErrorResponse("JSON parse error");
    }
    lastError_.clear();
    return response;
}

bool MCPClient::Initialize()
{
    if (isInitialized_) {
        return true;
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = nextRequestId_++;
    request["method"] = "initialize";
    request["params"]["protocolVersion"] = "2024-11-05";
    request["params"]["capabilities"] = nlohmann::json::object();
    request["params"]["clientInfo"]["name"] = name_;
    request["params"]["clientInfo"]["version"] = version_;

    nlohmann::json response = SendRequest(request);
    if (response.contains("error")) {
        lastError_ = response["error"].value("message", "Initialize failed");
        return false;
    }

    nlohmann::json notify;
    notify["jsonrpc"] = "2.0";
    notify["method"] = "notifications/initialized";
    notify["params"]["capabilities"] = nlohmann::json::object();
    SendRequest(notify);

    isInitialized_ = true;
    return true;
}

std::vector<MCPToolInfo> MCPClient::ListTools()
{
    std::vector<MCPToolInfo> tools;
    if (!isInitialized_) {
        lastError_ = "MCP client not initialized";
        return tools;
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = nextRequestId_++;
    request["method"] = "tools/list";
    request["params"] = nlohmann::json::object();

    nlohmann::json response = SendRequest(request);
    if (response.contains("error")) {
        lastError_ = response["error"].value("message", "List tools failed");
        return tools;
    }

    if (response.contains("result") && response["result"].contains("tools")) {
        for (const auto& toolDef : response["result"]["tools"]) {
            MCPToolInfo info;
            info.name = toolDef.value("name", "");
            info.description = toolDef.value("description", "");
            if (toolDef.contains("inputSchema")) {
                info.inputSchema = toolDef["inputSchema"];
            }
            tools.push_back(info);
        }
    }

    return tools;
}

std::shared_ptr<MCPToolResult> MCPClient::CallTool(const std::string& toolName, const nlohmann::json& arguments)
{
    auto result = std::make_shared<MCPToolResult>();
    if (!isInitialized_) {
        result->isError = true;
        result->content.push_back("MCP client not initialized");
        lastError_ = result->content.back();
        return result;
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = nextRequestId_++;
    request["method"] = "tools/call";
    request["params"]["name"] = toolName;
    request["params"]["arguments"] = arguments;

    nlohmann::json response = SendRequest(request);
    if (response.contains("error")) {
        result->isError = true;
        std::string message = response["error"].value("message", "Unknown error");
        result->content.push_back("Error: " + message);
        return result;
    }

    if (response.contains("result")) {
        auto& responseResult = response["result"];
        if (responseResult.contains("isError") && responseResult["isError"].is_boolean()) {
            result->isError = responseResult["isError"].get<bool>();
        }

        if (responseResult.contains("content") && responseResult["content"].is_array()) {
            for (const auto& contentItem : responseResult["content"]) {
                if (contentItem.contains("text")) {
                    result->content.push_back(contentItem["text"].get<std::string>());
                }
            }
        }
    }

    return result;
}

} // namespace jiuwen

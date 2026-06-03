#include "src/mcp/mcp_client.h"

#include <memory>
#include <string>
#include <vector>

#include "third_party/include/curl/curl.h"
#include "third_party/include/nlohmann/json.hpp"

namespace {

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output)
{
    size_t totalSize = size * nmemb;
    output->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

} // namespace

namespace jiuwen {

MCPClient::MCPClient(const std::string& name, const std::string& version, const std::string& endpoint)
    : name_(name), version_(version), endpoint_(endpoint)
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

    CURL* curl = curl_easy_init();
    if (!curl) {
        return MakeErrorResponse("Failed to initialize curl");
    }

    std::string requestBody = request.dump();
    std::string responseBody;

    curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(requestBody.size()));

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");

    std::string sessionSnapshot;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        sessionSnapshot = sessionId_;
    }
    if (!sessionSnapshot.empty()) {
        std::string sessionHeader = "Mcp-Session-Id: " + sessionSnapshot;
        headers = curl_slist_append(headers, sessionHeader.c_str());
    }

    for (const auto& header : headers_) {
        headers = curl_slist_append(headers, header.c_str());
    }

    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode curlResult = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (curlResult != CURLE_OK) {
        return MakeErrorResponse("CURL error: " + std::string(curl_easy_strerror(curlResult)));
    }

    if (httpCode != 200 && httpCode != 202) {
        std::string error = "HTTP error " + std::to_string(httpCode);
        if (!responseBody.empty()) {
            error += ": " + responseBody;
        }
        return MakeErrorResponse(error);
    }

    if (responseBody.empty()) {
        return nlohmann::json();
    }

    nlohmann::json response = nlohmann::json::parse(responseBody, nullptr, false);
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

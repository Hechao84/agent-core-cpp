#include "mcp_client.h"

#include <curl/curl.h>
#include <iostream>

namespace
{
    size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output)
    {
        size_t totalSize = size * nmemb;
        output->append(static_cast<char*>(contents), totalSize);
        return totalSize;
    }
} // namespace

MCPClient::MCPClient(const std::string& name, const std::string& version, const std::string& endpoint)
    : name_(name), version_(version), endpoint_(endpoint)
{
    if (endpoint_.empty())
    {
        throw std::invalid_argument("MCP endpoint must not be empty");
    }
}

MCPClient::~MCPClient() = default;

nlohmann::json MCPClient::SendRequest(const nlohmann::json& request)
{
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        throw std::runtime_error("Failed to initialize curl");
    }

    std::string requestBody = request.dump();
    std::string responseBody;

    curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(requestBody.size()));

    // Headers: Content-Type, Accept, Session-Id
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");

    if (!sessionId_.empty())
    {
        std::string sessionHeader = "Mcp-Session-Id: " + sessionId_;
        headers = curl_slist_append(headers, sessionHeader.c_str());
    }

    for (const auto& h : headers_)
    {
        headers = curl_slist_append(headers, h.c_str());
    }

    if (headers)
    {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        throw std::runtime_error("CURL error: " + std::string(curl_easy_strerror(res)));
    }

    if (httpCode != 200 && httpCode != 202)
    {
        std::string err = "HTTP error " + std::to_string(httpCode);
        if (!responseBody.empty())
        {
            err += ": " + responseBody;
        }
        throw std::runtime_error(err);
    }

    if (responseBody.empty())
    {
        return nlohmann::json();
    }

    try
    {
        return nlohmann::json::parse(responseBody);
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error("JSON parse error: " + std::string(e.what()));
    }
}

void MCPClient::Initialize()
{
    if (isInitialized_)
    {
        return;
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

    if (response.contains("error"))
    {
        std::string errMsg = response["error"].contains("message") ? response["error"]["message"].get<std::string>() : "";
        throw std::runtime_error("Initialize failed: " + errMsg);
    }

    if (response.contains("result"))
    {
        if (response["result"].contains("protocolVersion"))
        {
            // protocolVersion_ = response["result"]["protocolVersion"].get<std::string>();
        }
    }

    // Send initialized notification
    nlohmann::json notify;
    notify["jsonrpc"] = "2.0";
    notify["method"] = "notifications/initialized";
    notify["params"]["capabilities"] = nlohmann::json::object();

    try
    {
        SendRequest(notify);
    }
    catch (...)
    {
        // Ignore notification errors
    }

    isInitialized_ = true;
}

std::vector<MCPToolInfo> MCPClient::ListTools()
{
    if (!isInitialized_)
    {
        throw std::runtime_error("MCP client not initialized");
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = nextRequestId_++;
    request["method"] = "tools/list";
    request["params"] = nlohmann::json::object();

    nlohmann::json response = SendRequest(request);

    std::vector<MCPToolInfo> tools;
    if (response.contains("result") && response["result"].contains("tools"))
    {
        for (const auto& toolDef : response["result"]["tools"])
        {
            MCPToolInfo info;
            info.name = toolDef.value("name", "");
            info.description = toolDef.value("description", "");
            if (toolDef.contains("inputSchema"))
            {
                info.inputSchema = toolDef["inputSchema"];
            }
            tools.push_back(info);
        }
    }

    return tools;
}

std::shared_ptr<MCPToolResult> MCPClient::CallTool(const std::string& toolName, const nlohmann::json& arguments)
{
    if (!isInitialized_)
    {
        throw std::runtime_error("MCP client not initialized");
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = nextRequestId_++;
    request["method"] = "tools/call";
    request["params"]["name"] = toolName;
    request["params"]["arguments"] = arguments;

    nlohmann::json response = SendRequest(request);

    auto result = std::make_shared<MCPToolResult>();
    
    if (response.contains("error"))
    {
        result->isError = true;
        std::string msg = response["error"].value("message", "Unknown error");
        result->content.push_back("Error: " + msg);
    }
    else if (response.contains("result"))
    {
        auto& res = response["result"];
        if (res.contains("isError") && res["isError"].is_boolean())
        {
            result->isError = res["isError"].get<bool>();
        }

        if (res.contains("content") && res["content"].is_array())
        {
            for (const auto& c : res["content"])
            {
                if (c.contains("text"))
                {
                    result->content.push_back(c["text"].get<std::string>());
                }
            }
        }
    }

    return result;
}

#include "src/tools/builtin_tools/web_fetch_tool.h"
#include <iostream>
#include <string>
#include "src/utils/curl_client.h"

namespace jiuwen {

WebFetcherTool::WebFetcherTool() : Tool("web_fetcher", "Fetches the content of a given URL", {{"url", "The URL to fetch", "string", true}}){} std::string WebFetcherTool::FetchUrl(const std::string& url)
{
    CurlRequest req;
    req.url = url;
    req.followLocation = true;
    req.requestTimeout = 10;
    req.sslVerify = false;
    req.userAgent = "CppAgentFramework/1.0";

    CurlResponse resp = CurlClient::Get(req);
    if (resp.isCurlError) {
        return "Error: " + resp.curlErrorStr;
    }
    return resp.body;
}

std::string WebFetcherTool::Invoke(const std::string& input)
{
    // input is expected to be just the URL or JSON with "url" key
    std::string url = input;
    
    // Simple check if input is JSON {"url": "..."}
    size_t pos = input.find("\"url\"");
    if (pos != std::string::npos) {
        size_t start = input.find(":", pos) + 1;
        size_t end_quote = input.find("\"", input.find("\"", start) + 1); // Find the closing quote of value
        if (end_quote != std::string::npos) {
            url = input.substr(start + 1, end_quote - start - 1);
        }
    }
    
    return FetchUrl(url);
}

} // namespace jiuwen

#include "src/tools/builtin_tools/web_fetch_tool.h"
#include <iostream>
#include <string>
#include "src/utils/curl_client.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

using json = nlohmann::json;

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

std::string WebFetcherTool::ExtractUrl(const std::string& input)
{
    // The caller may pass either a bare URL or a JSON object {"url": "..."}.
    // Parse with nlohmann/json: a hand-rolled substring extractor previously
    // used here had an off-by-one that leaked the opening quote into the URL
    // string ("\"https://..."), which curl then rejected as
    // "URL using bad/illegal format or missing URL" on every fetch.
    std::string url = input;
    try {
        auto j = json::parse(input);
        if (j.is_object() && j.contains("url") && j["url"].is_string()) {
            url = j["url"].get<std::string>();
        }
    } catch (...) {
        // Not JSON — treat input as a raw URL.
    }
    return url;
}

std::string WebFetcherTool::Invoke(const std::string& input)
{
    return FetchUrl(ExtractUrl(input));
}

} // namespace jiuwen

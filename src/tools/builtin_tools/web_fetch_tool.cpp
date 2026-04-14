#include "web_fetch_tool.h"
#include "curl/curl.h"
#include <string>
#include <iostream>

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

WebFetcherTool::WebFetcherTool() : Tool("web_fetcher", "Fetches the content of a given URL", {{"url", "The URL to fetch", "string", true}}) {}

std::string WebFetcherTool::FetchUrl(const std::string& url)
{
    CURL* curl = curl_easy_init();
    if (!curl) return "Error: Failed to initialize curl";

    std::string readBuffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CppAgentFramework/1.0");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        return "Error: " + std::string(curl_easy_strerror(res));
    }
    curl_easy_cleanup(curl);
    return readBuffer;
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

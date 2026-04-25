#include "src/tools/builtin_tools/web_search_tool.h"
#include <iostream>
#include <string>
#include "third_party/include/curl/curl.h"

namespace jiuwen {

static size_t SearchWriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

WebSearchTool::WebSearchTool() : Tool("web_search", "Search the web for information", {{"query", "The search query", "string", true}}){} std::string WebSearchTool::Search(const std::string& query)
{
    CURL* curl = curl_easy_init();
    if (!curl) return "Error: Failed to initialize curl";

    // Encode query
    char* encoded_query = curl_easy_escape(curl, query.c_str(), (int)query.length());
    std::string url = "https://html.duckduckgo.com/html/?q=" + std::string(encoded_query);
    curl_free(encoded_query);

    std::string readBuffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, SearchWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        return "Error: " + std::string(curl_easy_strerror(res));
    }
    curl_easy_cleanup(curl);

    // Simple parsing to extract snippets (very basic for demo)
    // This is a mock parser for the purpose of the demo, as full HTML parsing in C++ is heavy.
    // We will search for class="result__snippet".
    std::string result;
    size_t pos = 0;
    while ((pos = readBuffer.find("class=\"result__snippet\"", pos)) != std::string::npos) {
        size_t start_tag = readBuffer.find(">", pos);
        size_t end_tag = readBuffer.find("</", start_tag);
        if (start_tag != std::string::npos && end_tag != std::string::npos) {
            std::string snippet = readBuffer.substr(start_tag + 1, end_tag - start_tag - 1);
            // Remove HTML tags within snippet
            result += "- " + snippet + "\n";
        }
        pos = end_tag;
    }
    
    if (result.empty()) return "No results found or parsing failed.";
    return result;
}

std::string WebSearchTool::Invoke(const std::string& input)
{
    std::string query = input;
    size_t pos = input.find("\"query\"");
    if (pos != std::string::npos) {
        size_t start = input.find(":", pos) + 1;
        size_t end_quote = input.find("\"", input.find("\"", start) + 1);
        if (end_quote != std::string::npos) {
            query = input.substr(start + 1, end_quote - start - 1);
        }
    }
    return Search(query);
}

} // namespace jiuwen

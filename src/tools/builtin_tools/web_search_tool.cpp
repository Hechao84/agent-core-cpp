#include "src/tools/builtin_tools/web_search_tool.h"

#include <algorithm>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "src/utils/curl_client.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

namespace {

constexpr const char* kChromeUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36";

struct HttpResponse {
    long status{0};
    std::string body;
    std::string err;
};

HttpResponse HttpGet(const std::string& url, int timeoutSec)
{
    HttpResponse out;
    CurlRequest req;
    req.url = url;
    req.headers = {
        "User-Agent: " + std::string(kChromeUserAgent),
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Language: en-US,en;q=0.9,zh-CN;q=0.8"};
    req.followLocation = true;
    req.requestTimeout = static_cast<long>(timeoutSec);
    req.connectTimeout = 10;
    req.sslVerify = false;

    CurlResponse resp = CurlClient::Get(req);
    out.status = resp.statusCode;
    out.body = std::move(resp.body);
    if (resp.isCurlError) {
        out.err = resp.curlErrorStr;
    }
    return out;
}

bool IsDDGChallenge(long status, const std::string& body)
{
    if (status == 202 || status == 418 || status == 429 || status == 503) {
        return true;
    }
    static const std::vector<std::string> markers = {
        "/anomaly.js",
        "challenge-form",
        "duckduckgo.com/anomaly.js",
    };
    for (const auto& m : markers) {
        if (body.find(m) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string HtmlDecodeBasic(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i + 1);
            if (semi != std::string::npos && semi - i <= 8) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                if (ent == "amp") { out += '&'; i = semi + 1; continue; }
                if (ent == "lt") { out += '<'; i = semi + 1; continue; }
                if (ent == "gt") { out += '>'; i = semi + 1; continue; }
                if (ent == "quot") { out += '"'; i = semi + 1; continue; }
                if (ent == "#39" || ent == "apos") { out += '\''; i = semi + 1; continue; }
                if (ent == "nbsp") { out += ' '; i = semi + 1; continue; }
            }
        }
        out += s[i++];
    }
    return out;
}

} // namespace

WebSearchTool::WebSearchTool()
    : Tool("web_search",
           "Search the web for information. Returns ranked results with title, URL and snippet. "
           "Tries DuckDuckGo first, falls back to Bing. "
           "Input JSON: {\"query\": <string required>, \"max_results\": <int optional 1-20, default 8>, "
           "\"timeout\": <int optional 5-30, default 15>}.",
           {{"query", "The search query", "string", true},
            {"max_results", "Maximum number of results (1-20, default 8)", "integer", false},
            {"timeout", "Per-engine timeout in seconds (5-30, default 15)", "integer", false}}) {}

std::string WebSearchTool::StripTags(const std::string& s)
{
    static const std::regex tagRe("<[^>]+>");
    static const std::regex wsRe("\\s+");
    std::string t = std::regex_replace(s, tagRe, " ");
    t = std::regex_replace(t, wsRe, " ");
    // Trim
    size_t a = t.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = t.find_last_not_of(" \t\r\n");
    return HtmlDecodeBasic(t.substr(a, b - a + 1));
}

std::string WebSearchTool::DecodeDDGRedirect(const std::string& href)
{
    // DDG returns links like //duckduckgo.com/l/?uddg=<encoded>&rut=...
    auto pos = href.find("uddg=");
    if (pos == std::string::npos) {
        // Sometimes href starts with // — normalize
        if (href.rfind("//", 0) == 0) return "https:" + href;
        return href;
    }
    pos += 5;
    auto end = href.find('&', pos);
    std::string encoded = href.substr(pos, end == std::string::npos ? std::string::npos : end - pos);

    return CurlClient::UrlDecode(encoded);
}

WebSearchTool::EngineOutcome WebSearchTool::SearchDuckDuckGo(const std::string& query,
                                                              int maxResults, int timeoutSec)
{
    EngineOutcome out;
    out.engine = "duckduckgo";
    std::string url = "https://html.duckduckgo.com/html/?q=" + CurlClient::UrlEncode(query);
    auto resp = HttpGet(url, timeoutSec);
    if (!resp.err.empty()) {
        out.errMsg = "ddg http error: " + resp.err;
        return out;
    }
    if (IsDDGChallenge(resp.status, resp.body)) {
        out.errMsg = "ddg anti-bot challenge (status " + std::to_string(resp.status) + ")";
        return out;
    }
    if (resp.status != 200) {
        out.errMsg = "ddg non-200 status: " + std::to_string(resp.status);
        return out;
    }

    // Use regex to extract anchors and snippets in order.
    // Patterns are lenient to tolerate attribute order changes.
    static const std::regex linkRe(
        R"(<a[^>]+class=\"result__a\"[^>]+href=\"([^\"]+)\"[^>]*>([\s\S]*?)</a>)",
        std::regex::icase);
    static const std::regex snippetRe(
        R"(<(?:a|div)[^>]+class=\"result__snippet\"[^>]*>([\s\S]*?)</(?:a|div)>)",
        std::regex::icase);

    std::vector<std::pair<std::string, std::string>> links; // href, title
    for (auto it = std::sregex_iterator(resp.body.begin(), resp.body.end(), linkRe);
         it != std::sregex_iterator(); ++it) {
        links.emplace_back((*it)[1].str(), (*it)[2].str());
    }
    std::vector<std::string> snippets;
    for (auto it = std::sregex_iterator(resp.body.begin(), resp.body.end(), snippetRe);
         it != std::sregex_iterator(); ++it) {
        snippets.push_back((*it)[1].str());
    }

    int count = (std::min)(static_cast<int>(links.size()), maxResults);
    for (int i = 0; i < count; ++i) {
        WebSearchResult r;
        r.title = StripTags(links[i].second);
        if (r.title.empty()) r.title = "Result " + std::to_string(i + 1);
        r.url = DecodeDDGRedirect(links[i].first);
        if (i < static_cast<int>(snippets.size())) {
            r.snippet = StripTags(snippets[i]);
        }
        out.rows.push_back(std::move(r));
    }

    if (out.rows.empty()) {
        out.errMsg = "ddg parsed zero results";
        return out;
    }
    out.ok = true;
    return out;
}

WebSearchTool::EngineOutcome WebSearchTool::SearchBing(const std::string& query,
                                                       int maxResults, int timeoutSec)
{
    EngineOutcome out;
    out.engine = "bing";
    std::string url = "https://www.bing.com/search?q=" + CurlClient::UrlEncode(query);
    auto resp = HttpGet(url, timeoutSec);
    if (!resp.err.empty()) {
        out.errMsg = "bing http error: " + resp.err;
        return out;
    }
    if (resp.status != 200) {
        out.errMsg = "bing non-200 status: " + std::to_string(resp.status);
        return out;
    }

    static const std::regex blockRe(
        R"(<li[^>]+class=\"[^\"]*\bb_algo\b[^\"]*\"[^>]*>([\s\S]*?)</li>)",
        std::regex::icase);
    static const std::regex titleRe(
        R"(<h2[^>]*>\s*<a[^>]+href=\"([^\"]+)\"[^>]*>([\s\S]*?)</a>)",
        std::regex::icase);
    static const std::regex snippetRe(R"(<p[^>]*>([\s\S]*?)</p>)", std::regex::icase);

    auto begin = std::sregex_iterator(resp.body.begin(), resp.body.end(), blockRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end && static_cast<int>(out.rows.size()) < maxResults; ++it) {
        std::string block = (*it)[1].str();
        std::smatch tm;
        if (!std::regex_search(block, tm, titleRe)) continue;
        WebSearchResult r;
        r.url = HtmlDecodeBasic(tm[1].str());
        r.title = StripTags(tm[2].str());
        if (r.title.empty()) r.title = "Result " + std::to_string(out.rows.size() + 1);
        std::smatch sm;
        if (std::regex_search(block, sm, snippetRe)) {
            r.snippet = StripTags(sm[1].str());
        }
        out.rows.push_back(std::move(r));
    }

    if (out.rows.empty()) {
        out.errMsg = "bing parsed zero results";
        return out;
    }
    out.ok = true;
    return out;
}

std::string WebSearchTool::Invoke(const std::string& input)
{
    std::string query;
    int maxResults = 8;
    int timeoutSec = 15;
    try {
        if (!input.empty() && input.front() == '{') {
            auto j = nlohmann::json::parse(input);
            if (j.contains("query") && j["query"].is_string()) {
                query = j["query"].get<std::string>();
            }
            if (j.contains("max_results") && j["max_results"].is_number_integer()) {
                maxResults = j["max_results"].get<int>();
            }
            if (j.contains("timeout") && j["timeout"].is_number_integer()) {
                timeoutSec = j["timeout"].get<int>();
            }
        } else {
            query = input;
        }
    } catch (const std::exception& e) {
        return std::string("Error: invalid JSON input: ") + e.what();
    }

    // Trim query
    size_t a = query.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) {
        return "Error: query is required and cannot be empty";
    }
    size_t b = query.find_last_not_of(" \t\r\n");
    query = query.substr(a, b - a + 1);

    if (maxResults < 1) maxResults = 1;
    if (maxResults > 20) maxResults = 20;
    if (timeoutSec < 5) timeoutSec = 5;
    if (timeoutSec > 30) timeoutSec = 30;

    std::vector<std::string> errs;

    EngineOutcome ddg = SearchDuckDuckGo(query, maxResults, timeoutSec);
    EngineOutcome chosen;
    if (ddg.ok) {
        chosen = std::move(ddg);
    } else {
        errs.push_back(ddg.engine + ": " + ddg.errMsg);
        EngineOutcome bing = SearchBing(query, maxResults, timeoutSec);
        if (bing.ok) {
            chosen = std::move(bing);
        } else {
            errs.push_back(bing.engine + ": " + bing.errMsg);
        }
    }

    if (!chosen.ok) {
        std::string msg = "Error: all search engines failed for query: " + query;
        for (const auto& e : errs) {
            msg += "\n  - " + e;
        }
        return msg;
    }

    std::ostringstream oss;
    oss << "Search results (" << chosen.engine << ") for: " << query << "\n";
    for (size_t i = 0; i < chosen.rows.size(); ++i) {
        const auto& r = chosen.rows[i];
        oss << (i + 1) << ". " << r.title << "\n";
        if (!r.url.empty()) oss << "   URL: " << r.url << "\n";
        if (!r.snippet.empty()) oss << "   Snippet: " << r.snippet << "\n";
    }
    return oss.str();
}

} // namespace jiuwen

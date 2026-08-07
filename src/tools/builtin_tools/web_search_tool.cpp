#include "src/tools/builtin_tools/web_search_tool.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "src/utils/curl_client.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

namespace {

// User-Agent pool. The whole process picks ONE entry from this pool at first
// use (see PickUserAgent) and reuses it for the entire process lifetime. Pool
// mixes Chrome and Firefox across Windows/Mac/Linux so a deploy on any of
// those hosts can pass UA-based origin checks engines still run.
constexpr std::array<const char*, 5> kUserAgents = {
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:127.0) "
    "Gecko/20100101 Firefox/127.0",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_5) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_5; rv:127.0) "
    "Gecko/20100101 Firefox/127.0",
};

// Per-engine cross-call cooldown. A search engine call within this many
// seconds of the previous call to the SAME engine blocks (sleeps the delta)
// before issuing the request. This is a per-process throttle keyed by engine
// name, not per-session — its job is to keep the *outbound IP* (often a
// shared VPN gateway) out of the engine's rate-limit window even when the
// agent issues back-to-back web_search calls with rephrased queries.
//
// Values are deliberately conservative: DDG flags the IP within ~6s of a
// prior call, so 8s puts us outside the window. Bing's Cloudflare is more
// aggressive and gets 15s. Baidu/Sogou tolerate higher rates from a China
// IP (no shared-VPN-exit problem since they're accessed direct), 5s/8s are
// conservative. Wikipedia's API is bot-friendly with its own generous
// limits, so 1s is just to avoid looking like a DoS.
constexpr int kDdgCooldownSec = 8;
constexpr int kBingCooldownSec = 15;
constexpr int kWikiCooldownSec = 1;
constexpr int kBaiduCooldownSec = 5;
constexpr int kSogouCooldownSec = 8;

constexpr int kMaxAttempts = 3;

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

// ---- Per-engine cross-call cooldown (Layer 1) ----
//
// Each engine has its own last-call timestamp in a process-global static.
// CooldownBefore enforces the minimum interval by sleeping the delta if the
// previous call was too recent. Thread-safe via a per-engine mutex (engines
// are independent so no global lock needed).
struct EngineCooldown {
    std::mutex mutex;
    std::chrono::steady_clock::time_point lastCall;
    int minIntervalSec;
};

EngineCooldown& CooldownFor(const std::string& engine)
{
    static EngineCooldown ddg{    {}, {}, kDdgCooldownSec};
    static EngineCooldown bing{   {}, {}, kBingCooldownSec};
    static EngineCooldown wiki{   {}, {}, kWikiCooldownSec};
    static EngineCooldown baidu{  {}, {}, kBaiduCooldownSec};
    static EngineCooldown sogou{  {}, {}, kSogouCooldownSec};
    if (engine == "duckduckgo") return ddg;
    if (engine == "bing") return bing;
    if (engine == "baidu") return baidu;
    if (engine == "sogou") return sogou;
    return wiki;  // default to wiki's loose interval
}

void CooldownBefore(const std::string& engine)
{
    EngineCooldown& cd = CooldownFor(engine);
    std::lock_guard<std::mutex> lock(cd.mutex);
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - cd.lastCall).count();
    if (elapsed < cd.minIntervalSec) {
        int sleepMs = (cd.minIntervalSec - static_cast<int>(elapsed)) * 1000;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
    cd.lastCall = std::chrono::steady_clock::now();
}

using ChallengeFn = std::function<bool(long, const std::string&)>;

// Single GET attempt. Wraps CurlClient::Get and maps the CurlResponse fields
// to WebSearchHttpResponse. Setting both userAgent (via CurlRequest field)
// and a "User-Agent:" header is intentional: the header is what the server
// sees, the field is what curl uses for its own UA-dependent behaviors (e.g.
// HTTP/2 negotiation, ALPN). They are kept in sync.
//
// noProxyHosts (optional): when non-empty, sets CURLOPT_NOPROXY so curl
// bypasses HTTPS_PROXY/HTTP_PROXY env vars for the listed hosts. Used by
// SearchBaidu to route through the local network (China IP) directly,
// avoiding the shared VPN exit IP that Western engines flag.
WebSearchHttpResponse HttpGetOnce(const std::string& url, int timeoutSec,
                                  const std::string& userAgent,
                                  const std::string& referer,
                                  bool enableCookies,
                                  const std::string& noProxyHosts = {})
{
    WebSearchHttpResponse out;
    CurlRequest req;
    req.url = url;
    req.headers = {
        "User-Agent: " + userAgent,
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Language: en-US,en;q=0.9,zh-CN;q=0.8"};
    req.followLocation = true;
    req.requestTimeout = static_cast<long>(timeoutSec);
    req.connectTimeout = 10;
    req.sslVerify = false;
    req.userAgent = userAgent;
    req.referer = referer;
    req.enableCookies = enableCookies;
    req.noProxyHosts = noProxyHosts;

    CurlResponse resp = CurlClient::Get(req);
    out.status = resp.statusCode;
    out.body = std::move(resp.body);
    out.isCurlError = resp.isCurlError;
    if (resp.isCurlError) {
        out.err = resp.curlErrorStr;
    }
    return out;
}

// Single POST attempt (form-encoded body). Used by DDG lite, which accepts
// POST form data as an alternative to GET querystring. Some anti-bot
// heuristics treat POST form submits as more "search-box-like" and less
// bot-like than GET querystrings; whether or not that's true, the variety
// alone is helpful.
WebSearchHttpResponse HttpPostOnce(const std::string& url, const std::string& body,
                                  int timeoutSec,
                                  const std::string& userAgent,
                                  const std::string& referer,
                                  bool enableCookies)
{
    WebSearchHttpResponse out;
    CurlRequest req;
    req.url = url;
    req.body = body;
    req.headers = {
        "User-Agent: " + userAgent,
        "Content-Type: application/x-www-form-urlencoded",
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Language: en-US,en;q=0.9,zh-CN;q=0.8"};
    req.followLocation = true;
    req.requestTimeout = static_cast<long>(timeoutSec);
    req.connectTimeout = 10;
    req.sslVerify = false;
    req.userAgent = userAgent;
    req.referer = referer;
    req.enableCookies = enableCookies;

    CurlResponse resp = CurlClient::Post(req);
    out.status = resp.statusCode;
    out.body = std::move(resp.body);
    out.isCurlError = resp.isCurlError;
    if (resp.isCurlError) {
        out.err = resp.curlErrorStr;
    }
    return out;
}

// Retry loop: up to kMaxAttempts (3) with exponential backoff (1s, 2s, 4s)
// between attempts. Retries ONLY when ShouldRetry returns true. Per Layer 2
// that means: curl transport errors and HTTP 429/503. Anti-bot challenges
// (202/418/anomaly.js/captcha/CfConfig) do NOT retry — they are hard bans
// requiring JS execution, and retrying just confirms bot detection and
// escalates the ban.
//
// noProxyHosts is plumbed through to HttpGetOnce for SearchBaidu's
// direct-connection bypass. Other callers leave it empty (default).
WebSearchHttpResponse HttpGetWithRetry(const std::string& url, int timeoutSec,
                                       const std::string& userAgent,
                                       const std::string& referer,
                                       bool enableCookies,
                                       ChallengeFn challengeFn,
                                       const std::string& noProxyHosts = {})
{
    WebSearchHttpResponse last;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        last = HttpGetOnce(url, timeoutSec, userAgent, referer, enableCookies, noProxyHosts);
        bool challenge = challengeFn ? challengeFn(last.status, last.body) : false;
        if (!ShouldRetry(last, challenge)) return last;
        if (attempt + 1 < kMaxAttempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ComputeBackoffMs(attempt)));
        }
    }
    return last;
}

} // namespace

// ---- Free function implementations (header-declared) ----

std::string PickUserAgent()
{
    // Per-process stable: pick one entry at first call, cache in function-local
    // static. std::random_device is the closest to "real" randomness C++ gives
    // us without a seed; the goal is just that two processes on the same host
    // don't both pick the same UA, not cryptographic strength.
    static const std::string ua = []() {
        std::random_device rd;
        return std::string(kUserAgents[rd() % kUserAgents.size()]);
    }();
    return ua;
}

bool ShouldRetry(const WebSearchHttpResponse& resp, bool /*challengeDetected*/)
{
    // Layer 2: fail-fast on real anti-bot challenges. challengeDetected is
    // intentionally ignored — body-marker challenges (anomaly.js, captcha,
    // CfConfig) require JS execution and will never be solved by plain-HTTP
    // retries. Retrying them just confirms bot detection and escalates the
    // ban, which is exactly the failure mode observed in production logs.
    // Only retry on true transient errors.
    if (resp.isCurlError) return true;
    if (resp.status == 429 || resp.status == 503) return true;
    return false;
}

int ComputeBackoffMs(int attempt)
{
    if (attempt < 0) return 0;
    if (attempt > 10) attempt = 10;  // defensive cap, prevents shift overflow
    return 1000 << attempt;
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

bool IsBingChallenge(long status, const std::string& body)
{
    if (status == 403 || status == 429 || status == 503) {
        return true;
    }
    // Bing's anti-bot surface is mostly Cloudflare Turnstile on a 200 body:
    // a `CfConfig` JS object plus a `class="captcha"` div plus a
    // `/challenge/verify` endpoint. BingBot / /identity/ are older markers
    // kept as defensive fallbacks. `bvs/` was removed: it appears in normal
    // result pages (Bing's voice-search telemetry) and would false-positive.
    static const std::vector<std::string> markers = {
        "CfConfig",
        "class=\"captcha\"",
        "/challenge/verify",
        "BingBot",
        "/identity/",
    };
    for (const auto& m : markers) {
        if (body.find(m) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool IsBaiduChallenge(long status, const std::string& body)
{
    if (status == 403 || status == 429 || status == 503) {
        return true;
    }
    // Baidu's anti-bot surface: 百度安全验证 / 百度验证 (verification pages),
    // waptcha (their captcha endpoint), safecheck (interstitial verification
    // page), 请输入验证码 (Chinese captcha prompt text), 网络不给力 (the
    // "network not strong" soft-challenge page Baidu serves when it suspects
    // bot traffic but doesn't want to outright block), /captcha/ endpoint.
    // Status 200 with these markers means a soft-challenge page was served
    // instead of real results.
    static const std::vector<std::string> markers = {
        "百度安全验证",
        "百度验证",
        "waptcha",
        "safecheck",
        "请输入验证码",
        "网络不给力",
        "/captcha/",
    };
    for (const auto& m : markers) {
        if (body.find(m) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool IsSogouChallenge(long status, const std::string& body)
{
    if (status == 403 || status == 429 || status == 503) {
        return true;
    }
    // Sogou's anti-bot: antispider endpoint + "用户您好" verification page +
    // "验证码" captcha prompt. Sogou is generally more permissive than
    // Bing/Brave for single requests, but does challenge under sustained
    // load.
    static const std::vector<std::string> markers = {
        "antispider",
        "用户您好",
        "请输入验证码",
        "/captcha/",
        "sogou_vr_captcha",
    };
    for (const auto& m : markers) {
        if (body.find(m) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool ContainsCJK(const std::string& query)
{
    // UTF-8 multibyte sequence decoder. CJK Unicode blocks covered:
    //   U+3000 - U+9FFF  : CJK symbols, Hiragana, Katakana, CJK Unified
    //   U+F900 - U+FAFF  : CJK Compatibility Ideographs
    //   U+FF00 - U+FFEF  : Halfwidth and Fullwidth Forms
    // All three live in the 3-byte UTF-8 range (codepoints U+0800 - U+FFFF),
    // so we look for the byte pattern 0xE3-0xEF for the first byte and
    // decode the codepoint from the three bytes.
    for (size_t i = 0; i < query.size();) {
        unsigned char c = static_cast<unsigned char>(query[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        if (c >= 0xE3 && c <= 0xEF && i + 2 < query.size()) {
            unsigned char b1 = static_cast<unsigned char>(query[i + 1]);
            unsigned char b2 = static_cast<unsigned char>(query[i + 2]);
            uint32_t cp = ((c & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
            if ((cp >= 0x3000 && cp <= 0x9FFF) ||
                (cp >= 0xF900 && cp <= 0xFAFF) ||
                (cp >= 0xFF00 && cp <= 0xFFEF)) {
                return true;
            }
            i += 3;
        } else if (c >= 0xC0 && c < 0xE0) {
            i += 2;  // 2-byte UTF-8 (Latin Extended, etc.) — not CJK
        } else if (c >= 0xF0) {
            i += 4;  // 4-byte UTF-8 (emoji, supplementary planes) — not CJK
        } else {
            ++i;  // invalid UTF-8 lead byte, skip
        }
    }
    return false;
}

std::vector<WebSearchResult> ParseDdgLiteResults(const std::string& body, int maxResults)
{
    std::vector<WebSearchResult> rows;

    // DDG lite anchor structure (stable across observed responses):
    //   <a rel="nofollow" href="//duckduckgo.com/l/?uddg=ENCODED&rut=..." class='result-link'>Title</a>
    // Attribute order is rel, href, class — the regex requires href before
    // class to disambiguate from other anchors on the page. The R"RE(...)RE"
    // custom delimiter is required because the pattern contains the literal
    // sequence `)"` inside a capture group, which would otherwise terminate
    // a plain R"(...)" raw string early.
    static const std::regex linkRe(
        R"RE(<a\s+[^>]*href="([^"]+)"[^>]*class='result-link'[^>]*>([\s\S]*?)</a>)RE",
        std::regex::icase);
    static const std::regex snippetRe(
        R"(<td\s+class='result-snippet'[^>]*>([\s\S]*?)</td>)",
        std::regex::icase);

    std::vector<std::pair<std::string, std::string>> links;  // href, title
    for (auto it = std::sregex_iterator(body.begin(), body.end(), linkRe);
         it != std::sregex_iterator(); ++it) {
        links.emplace_back((*it)[1].str(), (*it)[2].str());
    }
    std::vector<std::string> snippets;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), snippetRe);
         it != std::sregex_iterator(); ++it) {
        snippets.push_back((*it)[1].str());
    }

    int count = (std::min)(static_cast<int>(links.size()), maxResults);
    for (int i = 0; i < count; ++i) {
        WebSearchResult r;
        r.title = WebSearchTool::StripTags(links[i].second);
        if (r.title.empty()) r.title = "Result " + std::to_string(i + 1);
        r.url = WebSearchTool::DecodeDDGRedirect(links[i].first);
        if (i < static_cast<int>(snippets.size())) {
            r.snippet = WebSearchTool::StripTags(snippets[i]);
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<WebSearchResult> ParseBingResults(const std::string& body, int maxResults)
{
    std::vector<WebSearchResult> rows;

    static const std::regex blockRe(
        R"(<li[^>]+class=\"[^\"]*\bb_algo\b[^\"]*\"[^>]*>([\s\S]*?)</li>)",
        std::regex::icase);
    static const std::regex titleRe(
        R"(<h2[^>]*>\s*<a[^>]+href=\"([^\"]+)\"[^>]*>([\s\S]*?)</a>)",
        std::regex::icase);
    static const std::regex snippetRe(R"(<p[^>]*>([\s\S]*?)</p>)", std::regex::icase);

    auto begin = std::sregex_iterator(body.begin(), body.end(), blockRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end && static_cast<int>(rows.size()) < maxResults; ++it) {
        std::string block = (*it)[1].str();
        std::smatch tm;
        if (!std::regex_search(block, tm, titleRe)) continue;
        WebSearchResult r;
        r.url = HtmlDecodeBasic(tm[1].str());
        r.title = WebSearchTool::StripTags(tm[2].str());
        if (r.title.empty()) r.title = "Result " + std::to_string(rows.size() + 1);
        std::smatch sm;
        if (std::regex_search(block, sm, snippetRe)) {
            r.snippet = WebSearchTool::StripTags(sm[1].str());
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<WebSearchResult> ParseWikipediaResults(const std::string& jsonBody, int maxResults)
{
    std::vector<WebSearchResult> rows;
    if (jsonBody.empty()) return rows;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jsonBody);
    } catch (const std::exception&) {
        return rows;  // malformed JSON -> empty results
    }

    // Wikipedia response shape: {"query": {"search": [{"title","snippet",...}]}}
    if (!j.is_object() || !j.contains("query") || !j["query"].is_object()) return rows;
    const auto& q = j["query"];
    if (!q.contains("search") || !q["search"].is_array()) return rows;

    for (const auto& item : q["search"]) {
        if (static_cast<int>(rows.size()) >= maxResults) break;
        if (!item.is_object() || !item.contains("title") || !item["title"].is_string()) continue;
        WebSearchResult r;
        r.title = item["title"].get<std::string>();
        // Build the canonical article URL from the title (spaces -> underscores).
        // MediaWiki's API doesn't return the canonical URL directly in the
        // search list response; this construction matches Wikipedia's own
        // canonical article path.
        std::string titleForUrl = r.title;
        std::replace(titleForUrl.begin(), titleForUrl.end(), ' ', '_');
        r.url = "https://en.wikipedia.org/wiki/" + CurlClient::UrlEncode(titleForUrl);
        if (item.contains("snippet") && item["snippet"].is_string()) {
            // Snippet contains <span class="searchmatch">...</span> wrappers
            // around matched terms — StripTags handles them.
            r.snippet = WebSearchTool::StripTags(item["snippet"].get<std::string>());
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<WebSearchResult> ParseBaiduResults(const std::string& body, int maxResults)
{
    std::vector<WebSearchResult> rows;

    // Baidu's tn=baidurt legacy template result anchor:
    //   <h3 class="t"><a href="<external-URL>" ...>TITLE</a></h3>
    // (newer Baidu templates use class="c-title t"; the regex accepts any
    // h3 wrapper since the h3 itself is the discriminator that separates
    // real results from Baidu nav anchors — preferences/login/news/hao123
    // are typically in nav divs, not h3s). Requiring an h3 wrapper is more
    // robust than allowlisting/denylisting domains (hao123 is Baidu-owned
    // but on a different domain).
    //
    // The template returns DIRECT external URLs for results (e.g.
    // https://quote.eastmoney.com/...) — no baidu.com/link?url= redirect
    // hop needed. Some older result types still wrap in
    // http://www.baidu.com/link?url=... tracking redirects; the regex
    // accepts both forms. Snippets live in JS-rendered molecules and are
    // NOT extracted by this parser — the agent uses web_fetcher on the
    // result URL for details instead.
    static const std::regex titleRe(
        R"RE(<h3[^>]*>\s*<a[^>]+href="(https?://[^"]+)"[^>]*>([\s\S]{5,400}?)</a>)RE",
        std::regex::icase);

    for (auto it = std::sregex_iterator(body.begin(), body.end(), titleRe);
         it != std::sregex_iterator() && static_cast<int>(rows.size()) < maxResults; ++it) {
        std::string href = (*it)[1].str();
        // Defensive filter: even inside h3, skip Baidu-internal nav
        // (preferences/login/news.baidu.com/sf_vsearch) which would slip
        // through if Baidu changes its template structure. Keep
        // baidu.com/link?url=... redirects (real external results wrapped
        // in the legacy redirect hop).
        if (href.find("baidu.com") != std::string::npos &&
            href.find("/link?url=") == std::string::npos) {
            continue;
        }
        WebSearchResult r;
        r.url = href;
        r.title = WebSearchTool::StripTags((*it)[2].str());
        if (r.title.empty()) continue;
        // Snippet intentionally left empty — Baidu's baidurt template
        // JS-renders snippets into molecules, plain-HTML regex cannot
        // reliably extract them. The agent uses web_fetcher on r.url for
        // details.
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<WebSearchResult> ParseSogouResults(const std::string& body, int maxResults)
{
    std::vector<WebSearchResult> rows;

    // Sogou result structure (per vrwrap block):
    //   <div class="vrwrap" id="...">
    //     <h3 class="vr-title"><a name="dttl" href="/link?url=...">TITLE</a></h3>
    //     <div class="fz-mid space-txt ...">SNIPPET</div>
    //   </div>
    //
    // We avoid a lookahead-based blockRe (which causes std::regex stack
    // overflow on large bodies due to std::regex's recursive-descent
    // implementation of lookahead — confirmed via gdb backtrace showing
    // _M_dfs -> _M_lookahead -> _M_dfs recursion until SIGSEGV). Instead,
    // we scan the WHOLE body for title anchors and snippet divs separately,
    // then pair them by index (Sogou's HTML structure is consistent: each
    // vrwrap has exactly one title followed by exactly one snippet, so
    // i-th title ↔ i-th snippet by index).
    //
    // Title anchor carries name="dttl" and a /link?url= tracking href; the
    // tracking href is Sogou-internal (not the real external URL). We keep
    // the Sogou tracking URL as-is in r.url (matching how Bing tracking
    // URLs are kept) — decoding the real URL would require an extra HTTP
    // roundtrip.
    static const std::regex titleRe(
        R"RE(<a[^>]+name="dttl"[^>]+href="(/link\?url=[^"]+)"[^>]*>([\s\S]{5,400}?)</a>)RE",
        std::regex::icase);
    static const std::regex snippetRe(
        R"(class="fz-mid space-txt[^"]*"[^>]*>([\s\S]{5,1000}?)</div>)",
        std::regex::icase);

    std::vector<std::pair<std::string, std::string>> titles;  // href, title
    for (auto it = std::sregex_iterator(body.begin(), body.end(), titleRe);
         it != std::sregex_iterator(); ++it) {
        titles.emplace_back((*it)[1].str(), (*it)[2].str());
    }
    std::vector<std::string> snippets;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), snippetRe);
         it != std::sregex_iterator(); ++it) {
        snippets.push_back((*it)[1].str());
    }

    int count = (std::min)(static_cast<int>(titles.size()), maxResults);
    for (int i = 0; i < count; ++i) {
        WebSearchResult r;
        r.url = "https://www.sogou.com" + titles[i].first;  // absolute from relative
        r.title = WebSearchTool::StripTags(titles[i].second);
        if (r.title.empty()) r.title = "Result " + std::to_string(i + 1);
        if (i < static_cast<int>(snippets.size())) {
            r.snippet = WebSearchTool::StripTags(snippets[i]);
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

// ---- WebSearchResultCache (Layer 4) ----

WebSearchResultCache& WebSearchResultCache::Instance()
{
    static WebSearchResultCache instance;
    return instance;
}

const std::vector<WebSearchResult>* WebSearchResultCache::Lookup(const std::string& engine,
                                                                  const std::string& query)
{
    std::string key = engine + "|" + query;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return nullptr;
    if (std::chrono::steady_clock::now() >= it->second.expiresAt) {
        entries_.erase(it);
        return nullptr;
    }
    return &it->second.rows;
}

void WebSearchResultCache::Store(const std::string& engine, const std::string& query,
                                  std::vector<WebSearchResult> rows, int ttlSeconds)
{
    std::string key = engine + "|" + query;
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[key] = {std::move(rows),
                     std::chrono::steady_clock::now() + std::chrono::seconds(ttlSeconds)};
}

void WebSearchResultCache::Clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

// ---- WebSearchTool method implementations ----

WebSearchTool::WebSearchTool()
    : Tool("web_search",
           "Search the web for information. Returns ranked results with title, URL and snippet. "
           "For Chinese queries: tries Baidu (direct from local network) then Sogou then Wikipedia then Bing. "
           "For English/ASCII queries: tries DuckDuckGo then Wikipedia then Bing. "
           "Input JSON: {\"query\": <string required>, \"max_results\": <int optional 1-20, default 8>, "
           "\"timeout\": <int optional 5-30, default 15>}. "
           "Usage guidance: issue ONE web_search call per turn; for details on a result, "
           "use web_fetcher on its URL. Do NOT issue multiple web_search calls with "
           "rephrased queries in the same turn — that pattern looks like a bot to search "
           "engines and triggers anti-bot challenges that block the agent's IP for everyone.",
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
    // DDG (both html and lite endpoints) wraps result URLs in a redirect:
    //   //duckduckgo.com/l/?uddg=<encoded>&rut=...
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

    // Layer 4: cache hit short-circuits cooldown + network.
    if (const auto* cached = WebSearchResultCache::Instance().Lookup(out.engine, query)) {
        out.rows = *cached;
        out.ok = true;
        return out;
    }

    // Layer 1: enforce cross-call cooldown before issuing the request.
    CooldownBefore(out.engine);

    // Layer 1: POST form submit instead of GET querystring — variety helps.
    std::string url = "https://lite.duckduckgo.com/lite/";
    std::string body = "q=" + CurlClient::UrlEncode(query);
    std::string ua = PickUserAgent();
    auto resp = HttpPostOnce(url, body, timeoutSec, ua,
                            "https://lite.duckduckgo.com/", true);

    // HttpPostOnce does NOT retry (POST retry would re-POST, which the engine
    // might treat as duplicate submission). DDG's anti-bot is a 202/418/403
    // hard ban anyway — retrying would just confirm bot detection.
    if (resp.isCurlError) {
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

    out.rows = ParseDdgLiteResults(resp.body, maxResults);
    if (out.rows.empty()) {
        out.errMsg = "ddg parsed zero results";
        return out;
    }
    out.ok = true;
    WebSearchResultCache::Instance().Store(out.engine, query, out.rows);
    return out;
}

WebSearchTool::EngineOutcome WebSearchTool::SearchWikipedia(const std::string& query,
                                                             int maxResults, int timeoutSec)
{
    EngineOutcome out;
    out.engine = "wikipedia";

    if (const auto* cached = WebSearchResultCache::Instance().Lookup(out.engine, query)) {
        out.rows = *cached;
        out.ok = true;
        return out;
    }

    CooldownBefore(out.engine);

    // Wikipedia API: bot-friendly, JSON output, no anti-bot challenge by
    // design. origin=* enables CORS but also signals "any origin accepted"
    // which matches our use as a non-browser client.
    std::string url = "https://en.wikipedia.org/w/api.php?action=query&list=search"
                      "&srsearch=" + CurlClient::UrlEncode(query) +
                      "&format=json&srprop=snippet&srlimit=" + std::to_string(maxResults) +
                      "&origin=*";
    std::string ua = PickUserAgent();
    // Wikipedia's API tolerates high request rates, but we still benefit from
    // the retry path for transient transport errors and the occasional 429
    // from an upstream proxy. The challengeFn is nullptr — Wikipedia never
    // returns an anti-bot body, so no body-marker detection is needed.
    auto resp = HttpGetWithRetry(url, timeoutSec, ua, "https://en.wikipedia.org/", true, nullptr);

    if (resp.isCurlError) {
        out.errMsg = "wikipedia http error: " + resp.err;
        return out;
    }
    if (resp.status != 200) {
        out.errMsg = "wikipedia non-200 status: " + std::to_string(resp.status);
        return out;
    }

    out.rows = ParseWikipediaResults(resp.body, maxResults);
    if (out.rows.empty()) {
        // Wikipedia returns zero results when the query doesn't match any
        // article title. This is a normal "no coverage" outcome, not an
        // error — propagate as a soft failure so the caller falls through to
        // the next engine.
        out.errMsg = "wikipedia zero results";
        return out;
    }
    out.ok = true;
    WebSearchResultCache::Instance().Store(out.engine, query, out.rows);
    return out;
}

WebSearchTool::EngineOutcome WebSearchTool::SearchBaidu(const std::string& query,
                                                          int maxResults, int timeoutSec)
{
    EngineOutcome out;
    out.engine = "baidu";

    if (const auto* cached = WebSearchResultCache::Instance().Lookup(out.engine, query)) {
        out.rows = *cached;
        out.ok = true;
        return out;
    }

    CooldownBefore(out.engine);

    // Baidu: route direct from the local network (China IP) bypassing the
    // HTTPS_PROXY env var. The VPN exit IP is shared with other users and
    // gets rate-limited/flagged by Western engines; Baidu is reachable
    // from a China IP without a proxy and has better coverage for CJK
    // queries anyway. tn=baidurt forces the legacy HTML template that has
    // visible title text in <h3><a>...</a></h3> (the modern default
    // template JS-renders titles into empty anchors). ie=utf-8 makes Baidu
    // return UTF-8 instead of GBK.
    std::string url = "https://www.baidu.com/s?wd=" + CurlClient::UrlEncode(query) +
                      "&ie=utf-8&tn=baidurt&rn=" + std::to_string(maxResults);
    std::string ua = PickUserAgent();
    // noProxyHosts="baidu.com" → CURLOPT_NOPROXY bypasses HTTPS_PROXY env
    // var so curl connects via the local network's default route. For a
    // China-based host this hits Baidu directly from the China IP, not
    // through the shared VPN exit IP that Western engines flag.
    auto resp = HttpGetWithRetry(url, timeoutSec, ua,
                                 "https://www.baidu.com/", true,
                                 IsBaiduChallenge, "baidu.com");
    if (resp.isCurlError) {
        out.errMsg = "baidu http error: " + resp.err;
        return out;
    }
    if (IsBaiduChallenge(resp.status, resp.body)) {
        out.errMsg = "baidu anti-bot challenge (status " + std::to_string(resp.status) + ")";
        return out;
    }
    if (resp.status != 200) {
        out.errMsg = "baidu non-200 status: " + std::to_string(resp.status);
        return out;
    }

    out.rows = ParseBaiduResults(resp.body, maxResults);
    if (out.rows.empty()) {
        out.errMsg = "baidu parsed zero results";
        return out;
    }
    out.ok = true;
    WebSearchResultCache::Instance().Store(out.engine, query, out.rows);
    return out;
}

WebSearchTool::EngineOutcome WebSearchTool::SearchSogou(const std::string& query,
                                                          int maxResults, int timeoutSec)
{
    EngineOutcome out;
    out.engine = "sogou";

    if (const auto* cached = WebSearchResultCache::Instance().Lookup(out.engine, query)) {
        out.rows = *cached;
        out.ok = true;
        return out;
    }

    CooldownBefore(out.engine);

    // Sogou: routes through HTTPS_PROXY env var (the local Clash VPN) by
    // default — Sogou's anti-bot is more permissive than DDG/Bing/Brave and
    // the VPN exit IP is not yet flagged by Sogou in observed usage. If
    // Sogou starts challenging the VPN IP, callers should fall through to
    // Wikipedia (account-friendly, no IP-flag) and Bing (last resort).
    // ie=utf8&oe=utf8 forces UTF-8 input/output (Sogou defaults to GBK
    // otherwise, which would require charset conversion).
    std::string url = "https://www.sogou.com/web?query=" + CurlClient::UrlEncode(query) +
                      "&ie=utf8&oe=utf8&rn=" + std::to_string(maxResults);
    std::string ua = PickUserAgent();
    auto resp = HttpGetWithRetry(url, timeoutSec, ua,
                                 "https://www.sogou.com/", true,
                                 IsSogouChallenge);
    if (resp.isCurlError) {
        out.errMsg = "sogou http error: " + resp.err;
        return out;
    }
    if (IsSogouChallenge(resp.status, resp.body)) {
        out.errMsg = "sogou anti-bot challenge (status " + std::to_string(resp.status) + ")";
        return out;
    }
    if (resp.status != 200) {
        out.errMsg = "sogou non-200 status: " + std::to_string(resp.status);
        return out;
    }

    out.rows = ParseSogouResults(resp.body, maxResults);
    if (out.rows.empty()) {
        out.errMsg = "sogou parsed zero results";
        return out;
    }
    out.ok = true;
    WebSearchResultCache::Instance().Store(out.engine, query, out.rows);
    return out;
}

WebSearchTool::EngineOutcome WebSearchTool::SearchBing(const std::string& query,
                                                        int maxResults, int timeoutSec)
{
    EngineOutcome out;
    out.engine = "bing";

    if (const auto* cached = WebSearchResultCache::Instance().Lookup(out.engine, query)) {
        out.rows = *cached;
        out.ok = true;
        return out;
    }

    CooldownBefore(out.engine);

    std::string url = "https://www.bing.com/search?q=" + CurlClient::UrlEncode(query);
    std::string ua = PickUserAgent();
    auto resp = HttpGetWithRetry(url, timeoutSec, ua,
                                 "https://www.bing.com/", true,
                                 IsBingChallenge);
    if (resp.isCurlError) {
        out.errMsg = "bing http error: " + resp.err;
        return out;
    }
    if (IsBingChallenge(resp.status, resp.body)) {
        out.errMsg = "bing anti-bot challenge (status " + std::to_string(resp.status) + ")";
        return out;
    }
    if (resp.status != 200) {
        out.errMsg = "bing non-200 status: " + std::to_string(resp.status);
        return out;
    }

    out.rows = ParseBingResults(resp.body, maxResults);
    if (out.rows.empty()) {
        out.errMsg = "bing parsed zero results";
        return out;
    }
    out.ok = true;
    WebSearchResultCache::Instance().Store(out.engine, query, out.rows);
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

    // Engine order is query-language-routed (Layer 3''):
    // - CJK queries → Chinese engine chain ONLY: Baidu (direct from China
    //   IP, no VPN-exit-IP-flag problem) → Sogou (proxy, more permissive
    //   than DDG/Bing/Brave). STOPS HERE — Western engines (Wikipedia en,
    //   Bing) have poor or zero coverage for CJK queries and would only
    //   return garbage (e.g. Bing returns Walmart homepage for Chinese
    //   queries). If both Baidu and Sogou fail, return the IP-flagged
    //   error rather than wasting 2-3s on engines that can't help.
    // - ASCII queries → Western chain: DDG (best coverage when not blocked)
    //   → Wikipedia (entity coverage) → Bing (last resort). Baidu/Sogou
    //   have poor English coverage, so we skip them for ASCII queries.
    using EngineFn = std::function<EngineOutcome(const std::string&, int, int)>;
    std::vector<EngineFn> engines;
    if (ContainsCJK(query)) {
        engines = {
            [this](const std::string& q, int m, int t) { return SearchBaidu(q, m, t); },
            [this](const std::string& q, int m, int t) { return SearchSogou(q, m, t); },
        };
    } else {
        engines = {
            [this](const std::string& q, int m, int t) { return SearchDuckDuckGo(q, m, t); },
            [this](const std::string& q, int m, int t) { return SearchWikipedia(q, m, t); },
            [this](const std::string& q, int m, int t) { return SearchBing(q, m, t); },
        };
    }

    EngineOutcome chosen;
    for (auto& fn : engines) {
        auto outcome = fn(query, maxResults, timeoutSec);
        if (outcome.ok) {
            chosen = std::move(outcome);
            break;
        }
        errs.push_back(outcome.engine + ": " + outcome.errMsg);
    }

    if (!chosen.ok) {
        std::string msg = "Error: all search engines failed for query: " + query;
        for (const auto& e : errs) {
            msg += "\n  - " + e;
        }
        // Layer 5 hint embedded in the error so the model sees it without
        // depending on the tool description being in scope at error time.
        msg += "\nNote: the agent's outbound IP appears flagged by all "
               "engines. Consider waiting ~60s before retrying, using "
               "web_fetcher on a URL you already know, or rephrasing the "
               "query rather than re-searching with the same terms.";
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

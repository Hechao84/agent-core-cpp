#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "include/tool.h"

namespace jiuwen {

struct WebSearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

class WebSearchTool : public Tool {
public:
    WebSearchTool();
    std::string Invoke(const std::string& input) override;

    // Tag stripper and DDG redirect decoder are exposed as public static
    // methods so the per-engine HTML parsers (ParseDdgLiteResults /
    // ParseBingResults, declared below as free functions) can reuse them
    // without an instance. Both are pure string transforms.
    static std::string StripTags(const std::string& s);
    static std::string DecodeDDGRedirect(const std::string& href);

private:
    // Returns ok=true with results on success; ok=false with errMsg on failure
    // (including anti-bot challenge pages). Engine name records which backend
    // produced the rows.
    struct EngineOutcome {
        bool ok{false};
        std::string errMsg;
        std::vector<WebSearchResult> rows;
        std::string engine;
    };
    EngineOutcome SearchDuckDuckGo(const std::string& query, int maxResults, int timeoutSec);
    EngineOutcome SearchBaidu(const std::string& query, int maxResults, int timeoutSec);
    EngineOutcome SearchSogou(const std::string& query, int maxResults, int timeoutSec);
    EngineOutcome SearchWikipedia(const std::string& query, int maxResults, int timeoutSec);
    EngineOutcome SearchBing(const std::string& query, int maxResults, int timeoutSec);
};

// ---- Internal helpers (file-local in .cpp, declared here for unit tests) ----
//
// These free functions implement the retry policy, UA picking, per-engine
// anti-bot challenge detection, and per-engine HTML/JSON parsing used by
// WebSearchTool. They are kept as free functions (rather than private static
// members) so unit tests can verify the policy and parser logic without going
// to the network. The retry loop itself (HttpGetWithRetry in the .cpp) is not
// exposed: it just orchestrates these pure functions plus CurlClient::Get and
// is covered end-to-end by integration_tests/smoke_web_search.cpp.

// HTTP response as seen by the retry layer. isCurlError is true when the
// transport itself failed (CURLE_*); in that case err holds the curl
// strerror and status is whatever (often 0) curl reported.
struct WebSearchHttpResponse {
    long status{0};
    std::string body;
    std::string err;
    bool isCurlError{false};
};

// Per-process stable User-Agent pick. Process-global: the first call picks
// one UA from a 5-entry pool (Chrome / Firefox across Windows / Mac / Linux)
// using std::random_device and caches it in a function-local static; all
// subsequent calls return the same UA. This looks like one user with one
// browser across the entire process lifetime, avoiding the suspicious
// "same IP cycling UAs between calls" pattern that per-query rotation causes.
std::string PickUserAgent();

// Pure retry-policy decision. Returns true if the response warrants a retry.
// Retries ONLY on: curl transport errors (CURLE_*) and HTTP 429/503 (true
// transient statuses where retry-after-backoff is the right move).
//
// Deliberately does NOT retry on:
//   - 202/418/403 (DDG/Bing "you must solve a JS challenge" hard bans —
//     retrying just confirms bot detection and escalates the ban)
//   - body-marker challenges (anomaly.js / captcha / CfConfig — these need
//     JS execution, no amount of plain-HTTP retries will pass)
//
// challengeDetected is the caller's per-engine body-marker check result
// (IsDDGChallenge / IsBingChallenge). It is included in the signature so
// ShouldRetry stays the single retry decision point, but its presence does
// NOT trigger a retry — only curl errors and 429/503 do.
bool ShouldRetry(const WebSearchHttpResponse& resp, bool challengeDetected);

// Exponential backoff in milliseconds: 1000 << attempt -> 1s, 2s, 4s, ...
// Capped at attempt=10 (~17min) to prevent shift overflow; the caller's
// maxAttempts controls the actual retry ceiling so the cap is defensive.
int ComputeBackoffMs(int attempt);

// HTML parsers for each engine. Pure: take body, return rows. Used by
// WebSearchTool's engine methods and unit-tested with fixture HTML under
// unittest/data/web_search_*.html.
std::vector<WebSearchResult> ParseDdgLiteResults(const std::string& body, int maxResults);
std::vector<WebSearchResult> ParseBingResults(const std::string& body, int maxResults);

// JSON parser for the Wikipedia API response. Wikipedia's API returns
// {"query": {"search": [{"title","snippet","pageid",...}, ...]}}. Pure:
// takes the raw JSON body, returns rows with url built as
// https://en.wikipedia.org/wiki/<title-with-underscores>. Snippet is
// returned with HTML searchmatch spans stripped via StripTags. Empty body,
// malformed JSON, or zero results all return an empty vector.
std::vector<WebSearchResult> ParseWikipediaResults(const std::string& jsonBody, int maxResults);

// Baidu SERP parser (tn=baidurt legacy template). The baidurt template
// returns HTML with real title text in <h3 class="c-title t"><a href>...
// (the modern default template JS-renders titles into empty <a> tags,
// unparseable by plain-HTML regex; baidurt is the programmatic-friendly
// fallback). Snippets live in JS-rendered molecules in the baidurt template
// too and are NOT extracted by this parser — the agent uses web_fetcher on
// the result URL for details. Only anchors whose href matches
// http://www.baidu.com/link?url=... are kept (internal Baidu vertical
// links like /sf/vsearch and homepage nav links are filtered out).
std::vector<WebSearchResult> ParseBaiduResults(const std::string& body, int maxResults);

// Sogou SERP parser. Each result lives in <div class="vrwrap"> with the
// title anchor carrying name="dddl" and a /link?url= tracking href, plus
// a sibling <div class="fz-mid space-txt ..."> snippet. Snippet IS
// extractable (unlike Baidu's JS-rendered molecules) and is included.
std::vector<WebSearchResult> ParseSogouResults(const std::string& body, int maxResults);

// Engine-specific anti-bot challenge detectors. Each inspects both the HTTP
// status (transient anti-bot codes) and the body (well-known challenge
// markers). Pure so unit tests can feed fixture bodies directly.
bool IsDDGChallenge(long status, const std::string& body);
bool IsBingChallenge(long status, const std::string& body);
bool IsBaiduChallenge(long status, const std::string& body);
bool IsSogouChallenge(long status, const std::string& body);

// Detects whether a query string contains CJK Unicode characters (Hiragana,
// Katakana, CJK Unified Ideographs, CJK Compatibility Ideographs, Halfwidth
// and Fullwidth Forms). Used by WebSearchTool::Invoke to route CJK queries
// to the Chinese search engine chain (Baidu/Sogou direct from China IP,
// avoiding the shared VPN exit IP that Western engines flag) and ASCII-only
// queries to the Western chain (DDG/Wikipedia/Bing). Pure, UTF-8 aware.
bool ContainsCJK(const std::string& query);

// ---- In-memory result cache (Layer 4) ----
//
// Keyed by "<engine>|<query>", TTL 5 minutes. Hits short-circuit both the
// per-engine cooldown and the network roundtrip. Lives as a process-global
// singleton (one cache shared across all sessions/tools); thread-safe via
// internal mutex. Only successful engine outcomes are cached; failures are
// never cached (so a transient IP-flag doesn't poison subsequent calls).
//
// Exposed as a class so unit tests can construct an instance with a short
// TTL and verify hit/miss/expiry without sleeping for 5 minutes.
class WebSearchResultCache {
public:
    static WebSearchResultCache& Instance();

    // Lookup a cached entry. Returns nullptr on miss or expired entry.
    // Expiry removes the entry as a side effect (lazy eviction).
    const std::vector<WebSearchResult>* Lookup(const std::string& engine,
                                                const std::string& query);

    // Store an entry. ttlSeconds defaults to 300 (5 min); tests can pass a
    // short TTL to verify expiry without sleeping.
    void Store(const std::string& engine, const std::string& query,
               std::vector<WebSearchResult> rows, int ttlSeconds = 300);

    // Test-only: clear all entries.
    void Clear();

private:
    WebSearchResultCache() = default;
    struct Entry {
        std::vector<WebSearchResult> rows;
        std::chrono::steady_clock::time_point expiresAt;
    };
    std::mutex mutex_;
    std::map<std::string, Entry> entries_;
};

}  // namespace jiuwen

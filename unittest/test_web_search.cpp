#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "src/tools/builtin_tools/web_search_tool.h"
#include "test_runner.h"

using namespace jiuwen;

namespace {

// Read a fixture HTML/JSON file from unittest/data/. main.cpp chdir's to
// the project root before running tests, so the relative path resolves the
// same regardless of where the test binary is invoked from.
std::string LoadFixture(const std::string& relPath)
{
    std::ifstream f(relPath, std::ios::binary);
    if (!f) {
        throw std::runtime_error("fixture not found: " + relPath);
    }
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

} // namespace

// ---- Parser tests (fixture-driven) ----

TEST(web_search, ParseDdgLiteResultsParsesResults)
{
    auto body = LoadFixture("unittest/data/web_search_ddg_lite.html");
    auto rows = ParseDdgLiteResults(body, 8);
    TestRunner::AssertEq(rows.size(), static_cast<size_t>(3),
        "DDG lite fixture has 3 results");
    TestRunner::AssertEq(rows[0].title, std::string("SQL Playground"),
        "first DDG lite title");
    TestRunner::AssertEq(rows[0].url, std::string("https://sqltutorial.org/playground/"),
        "first DDG lite URL after DecodeDDGRedirect");
    TestRunner::AssertContains(rows[0].snippet, "free and interactive",
        "first DDG lite snippet");
    TestRunner::AssertContains(rows[1].title, "SQL Fiddle",
        "second DDG lite title");
    TestRunner::AssertEq(rows[2].title, std::string("SQL Test"),
        "third DDG lite title");
}

TEST(web_search, ParseDdgLiteResultsEmptyBody)
{
    auto rows = ParseDdgLiteResults("", 8);
    TestRunner::AssertEq(rows.size(), static_cast<size_t>(0),
        "empty body yields zero rows");
}

TEST(web_search, ParseDdgLiteResultsRespectsMaxResults)
{
    auto body = LoadFixture("unittest/data/web_search_ddg_lite.html");
    auto rows = ParseDdgLiteResults(body, 2);
    TestRunner::AssertEq(rows.size(), static_cast<size_t>(2),
        "maxResults caps row count");
}

TEST(web_search, ParseBingResultsParsesResults)
{
    auto body = LoadFixture("unittest/data/web_search_bing.html");
    auto rows = ParseBingResults(body, 8);
    TestRunner::AssertEq(rows.size(), static_cast<size_t>(3),
        "Bing fixture has 3 results");
    TestRunner::AssertEq(rows[0].title, std::string("SQL Playground"),
        "first Bing title");
    TestRunner::AssertStartsWith(rows[0].url, "https://www.bing.com/ck/a?",
        "first Bing URL is the Bing tracking href");
    TestRunner::AssertContains(rows[0].snippet, "free and interactive",
        "first Bing snippet");
    TestRunner::AssertContains(rows[1].title, "SQL Fiddle",
        "second Bing title");
}

TEST(web_search, ParseBingResultsEmptyBody)
{
    auto rows = ParseBingResults("", 8);
    TestRunner::AssertEq(rows.size(), static_cast<size_t>(0),
        "empty body yields zero rows");
}

// ---- Baidu parser tests (fixture-driven) ----

TEST(web_search, ParseBaiduResultsParsesResults)
{
    auto body = LoadFixture("unittest/data/web_search_baidu.html");
    auto rows = ParseBaiduResults(body, 8);
    TestRunner::AssertEq(rows.size(), static_cast<size_t>(3),
        "Baidu fixture has 3 results");
    TestRunner::AssertContains(rows[0].title, "长鑫科技",
        "first Baidu title (with <em> stripped)");
    TestRunner::AssertEq(rows[0].url,
        std::string("https://quote.eastmoney.com/concept/sh688825.html"),
        "first Baidu URL is the DIRECT external URL (Baidu's baidurt "
        "template returns external URLs, not baidu.com/link?url= redirects)");
    // Snippets intentionally not extracted (Baidu's baidurt template
    // JS-renders snippets into molecules — agent uses web_fetcher on URL).
    TestRunner::AssertTrue(rows[0].snippet.empty(),
        "Baidu snippet is empty by design (use web_fetcher for details)");
}

TEST(web_search, ParseBaiduResultsEmptyBody)
{
    auto rows = ParseBaiduResults("", 8);
    TestRunner::AssertEq(rows.size(), static_cast<size_t>(0),
        "empty body yields zero rows");
}

TEST(web_search, ParseBaiduResultsFiltersNonResultAnchors)
{
    // The fixture includes Baidu-internal anchors (preferences, login,
    // news.baidu.com nav, hao123 homepage nav, /sf/vsearch video vertical).
    // The parser must keep ONLY anchors whose href is either a direct
    // external URL OR a baidu.com/link?url=... redirect — not internal
    // Baidu pages.
    auto body = LoadFixture("unittest/data/web_search_baidu.html");
    auto rows = ParseBaiduResults(body, 8);
    for (const auto& r : rows) {
        bool isExternalUrl = (r.url.find("http://") == 0 || r.url.find("https://") == 0);
        bool isBaiduRedirect = r.url.find("baidu.com/link?url=") != std::string::npos;
        bool isInternalBaidu = r.url.find("baidu.com") != std::string::npos && !isBaiduRedirect;
        TestRunner::AssertTrue(isExternalUrl && !isInternalBaidu,
            "every parsed URL must be either a direct external URL or "
            "a baidu.com/link?url= redirect, NOT an internal Baidu page "
            "(preferences/login/news/hao123/vsearch)");
    }
}

// ---- Sogou parser tests (fixture-driven) ----

TEST(web_search, ParseSogouResultsParsesResults)
{
    auto body = LoadFixture("unittest/data/web_search_sogou.html");
    auto rows = ParseSogouResults(body, 8);
    TestRunner::AssertEq(rows.size(), static_cast<size_t>(3),
        "Sogou fixture has 3 results");
    TestRunner::AssertContains(rows[0].title, "结果一",
        "first Sogou title (with <em> stripped)");
    TestRunner::AssertStartsWith(rows[0].url, "https://www.sogou.com/link?url=",
        "first Sogou URL is the sogou.com tracking href (absolute from relative)");
    TestRunner::AssertContains(rows[0].snippet, "摘要",
        "first Sogou snippet extracted");
}

TEST(web_search, ParseSogouResultsEmptyBody)
{
    auto rows = ParseSogouResults("", 8);
    TestRunner::AssertEq(rows.size(), static_cast<size_t>(0),
        "empty body yields zero rows");
}

// ---- CJK detection tests ----

TEST(web_search, ContainsCJKDetectsChinese)
{
    TestRunner::AssertTrue(ContainsCJK("今天有什么有趣的事情"),
        "Chinese text contains CJK");
    TestRunner::AssertTrue(ContainsCJK("长鑫科技 688825 今日股价"),
        "Mixed Chinese + ASCII still flagged as CJK");
    TestRunner::AssertTrue(ContainsCJK("日本語のテスト"),
        "Japanese hiragana/katakana count as CJK");
    // Korean Hangul Syllables (U+AC00-U+D7AF) are NOT in the detector's
    // covered ranges (only U+3000-U+9FFF + compat + halfwidth). This gap
    // is intentional — the agent's primary use case is Chinese, and adding
    // Hangul coverage is a separate change if Korean queries become a
    // target. The assertion documents the gap as expected behavior.
    TestRunner::AssertFalse(ContainsCJK("한국어"),
        "Korean Hangul is out of detector scope (gap is intentional)");
}

TEST(web_search, ContainsCJKRejectsAscii)
{
    TestRunner::AssertFalse(ContainsCJK("test query"),
        "Pure ASCII contains no CJK");
    TestRunner::AssertFalse(ContainsCJK("ChangXin Memory Technologies"),
        "English company name has no CJK");
    TestRunner::AssertFalse(ContainsCJK(""),
        "empty string has no CJK");
    TestRunner::AssertFalse(ContainsCJK("1234567890 !@#$%^&*()"),
        "digits and punctuation have no CJK");
}

TEST(web_search, ContainsCJKHandlesMixedContent)
{
    // The query "长鑫科技 688825" has CJK + ASCII — CJK should win.
    TestRunner::AssertTrue(ContainsCJK("长鑫科技 688825"),
        "CJK + ASCII mix routes to Chinese engine chain");
    // Latin Extended (é, ü) is 2-byte UTF-8, NOT CJK — must NOT trigger.
    TestRunner::AssertFalse(ContainsCJK("café résumé naïve"),
        "Latin Extended diacritics are 2-byte UTF-8, not CJK");
    // Emoji is 4-byte UTF-8 (U+1F000+), NOT CJK — must NOT trigger.
    TestRunner::AssertFalse(ContainsCJK("hello 🚀 world"),
        "emoji is 4-byte UTF-8, not CJK");
}

// ---- Baidu/Sogou challenge detector tests ----

TEST(web_search, IsBaiduChallengeDetectsMarkers)
{
    TestRunner::AssertTrue(IsBaiduChallenge(200, "<html><title>百度安全验证</title></html>"),
        "body with '百度安全验证' marker (Baidu's actual soft-challenge page title) is a challenge");
    TestRunner::AssertTrue(IsBaiduChallenge(200, "<html><title>百度验证</title></html>"),
        "body with '百度验证' marker is a challenge");
    TestRunner::AssertTrue(IsBaiduChallenge(200, "网络不给力，请稍后重试"),
        "body with '网络不给力' (Baidu's soft-challenge 'network not strong' page) is a challenge");
    TestRunner::AssertTrue(IsBaiduChallenge(200, "url=/captcha/?token=..."),
        "body with /captcha/ marker is a challenge");
    TestRunner::AssertTrue(IsBaiduChallenge(403, ""),
        "403 is a Baidu challenge status");
    TestRunner::AssertTrue(IsBaiduChallenge(429, ""),
        "429 is a Baidu challenge status");
    TestRunner::AssertFalse(IsBaiduChallenge(200, "<html><body>normal results</body></html>"),
        "normal result body is not a challenge");
}

TEST(web_search, IsSogouChallengeDetectsMarkers)
{
    TestRunner::AssertTrue(IsSogouChallenge(200, "<html><body>antispider</body></html>"),
        "body with 'antispider' marker is a challenge");
    TestRunner::AssertTrue(IsSogouChallenge(200, "请输入验证码"),
        "body with '请输入验证码' marker is a challenge");
    TestRunner::AssertTrue(IsSogouChallenge(403, ""),
        "403 is a Sogou challenge status");
    TestRunner::AssertFalse(IsSogouChallenge(200, "<html><body>normal results</body></html>"),
        "normal result body is not a challenge");
}

// ---- Wikipedia parser tests (fixture-driven) ----

TEST(web_search, ParseWikipediaResultsParsesResults)
{
    auto body = LoadFixture("unittest/data/web_search_wikipedia.json");
    auto rows = ParseWikipediaResults(body, 8);
    TestRunner::AssertEq(rows.size(), static_cast<size_t>(3),
        "Wikipedia fixture has 3 results");
    TestRunner::AssertEq(rows[0].title, std::string("ChangXin Memory Technologies"),
        "first Wikipedia title");
    TestRunner::AssertStartsWith(rows[0].url, "https://en.wikipedia.org/wiki/",
        "Wikipedia URL is the canonical article path");
    // Title with spaces -> underscores in the URL.
    TestRunner::AssertContains(rows[0].url, "ChangXin_Memory_Technologies",
        "spaces in title become underscores in URL");
    // Snippet has searchmatch spans stripped.
    TestRunner::AssertFalse(rows[0].snippet.find("<span") != std::string::npos,
        "snippet HTML spans stripped");
    TestRunner::AssertContains(rows[0].snippet, "ChangXin",
        "snippet text content preserved");
}

TEST(web_search, ParseWikipediaResultsEmptyBody)
{
    auto rows = ParseWikipediaResults("", 8);
    TestRunner::AssertEq(rows.size(), static_cast<size_t>(0),
        "empty body yields zero rows");
}

TEST(web_search, ParseWikipediaResultsMalformedJson)
{
    auto rows = ParseWikipediaResults("not json {", 8);
    TestRunner::AssertEq(rows.size(), static_cast<size_t>(0),
        "malformed JSON yields zero rows (no exception thrown)");
}

TEST(web_search, ParseWikipediaResultsZeroResults)
{
    // Wikipedia returns 200 with empty search array when no articles match.
    // This is a normal "no coverage" outcome, not an error.
    std::string body = R"({"query":{"search":[],"searchinfo":{"totalhits":0}}})";
    auto rows = ParseWikipediaResults(body, 8);
    TestRunner::AssertEq(rows.size(), static_cast<size_t>(0),
        "zero results in body yields zero rows");
}

TEST(web_search, ParseWikipediaResultsRespectsMaxResults)
{
    auto body = LoadFixture("unittest/data/web_search_wikipedia.json");
    auto rows = ParseWikipediaResults(body, 2);
    TestRunner::AssertEq(rows.size(), static_cast<size_t>(2),
        "maxResults caps row count");
}

// ---- Challenge detector tests (fixture-driven) ----

TEST(web_search, IsDDGChallengeDetectsAnomalyJs)
{
    auto body = LoadFixture("unittest/data/web_search_ddg_challenge.html");
    TestRunner::AssertTrue(IsDDGChallenge(200, body),
        "body with /anomaly.js + challenge-form markers is a challenge");
}

TEST(web_search, IsDDGChallengeTransientStatus)
{
    TestRunner::AssertTrue(IsDDGChallenge(202, ""),
        "202 is a DDG challenge status");
    TestRunner::AssertTrue(IsDDGChallenge(429, ""),
        "429 is a DDG challenge status");
    TestRunner::AssertTrue(IsDDGChallenge(503, ""),
        "503 is a DDG challenge status");
    TestRunner::AssertFalse(IsDDGChallenge(200, ""),
        "200 with no markers is not a challenge");
}

TEST(web_search, IsBingChallengeDetectsCaptchaMarkers)
{
    auto body = LoadFixture("unittest/data/web_search_bing_challenge.html");
    TestRunner::AssertTrue(IsBingChallenge(200, body),
        "body with CfConfig + captcha div is a Bing challenge");
}

TEST(web_search, IsBingChallengeTransientStatus)
{
    TestRunner::AssertTrue(IsBingChallenge(403, ""),
        "403 is a Bing challenge status");
    TestRunner::AssertTrue(IsBingChallenge(429, ""),
        "429 is a Bing challenge status");
    TestRunner::AssertTrue(IsBingChallenge(503, ""),
        "503 is a Bing challenge status");
    TestRunner::AssertFalse(IsBingChallenge(200, ""),
        "200 with no markers is not a challenge");
}

TEST(web_search, IsBingChallengeDoesNotFalsePositiveOnResultPage)
{
    auto body = LoadFixture("unittest/data/web_search_bing.html");
    TestRunner::AssertFalse(IsBingChallenge(200, body),
        "clean Bing SERP body must not be flagged as a challenge");
}

// ---- Retry policy pure-function tests ----
//
// Layer 2 (fail-fast on challenges): ShouldRetry returns true ONLY for curl
// transport errors and HTTP 429/503 (true transient statuses). All anti-bot
// challenge statuses (202/418/403) and body-marker challenges do NOT retry
// — they are hard bans requiring JS execution, and retrying just confirms
// bot detection and escalates the ban (the exact failure mode observed in
// production).

TEST(web_search, ShouldRetryRetriesOnTrueTransientStatus)
{
    WebSearchHttpResponse r;
    r.isCurlError = false;
    r.status = 429;
    TestRunner::AssertTrue(ShouldRetry(r, false), "429 retries");
    r.status = 503;
    TestRunner::AssertTrue(ShouldRetry(r, false), "503 retries");
}

TEST(web_search, ShouldRetryDoesNotRetryOnChallengeStatus)
{
    // These statuses are anti-bot hard bans, not transient. Retrying them
    // is the wrong move (was the production failure mode before Layer 2).
    WebSearchHttpResponse r;
    r.isCurlError = false;
    r.status = 202;  // DDG anomaly.js challenge
    TestRunner::AssertFalse(ShouldRetry(r, false), "202 is a hard ban, not transient");
    r.status = 418;  // DDG I'm-a-teapot anti-bot
    TestRunner::AssertFalse(ShouldRetry(r, false), "418 is a hard ban, not transient");
    r.status = 403;  // Bing Cloudflare block
    TestRunner::AssertFalse(ShouldRetry(r, false), "403 is a hard ban, not transient");
}

TEST(web_search, ShouldRetryDoesNotRetryOnChallengeBodyEvenWhenChallengeDetected)
{
    // challengeDetected is intentionally IGNORED by ShouldRetry per Layer 2:
    // body-marker challenges require JS execution and will never be solved
    // by plain-HTTP retries. The signature keeps challengeDetected for API
    // stability and to keep ShouldRetry as the single retry decision point,
    // but its value does not affect the result.
    WebSearchHttpResponse r;
    r.isCurlError = false;
    r.status = 200;  // Bing returns 200 with a captcha body
    TestRunner::AssertFalse(ShouldRetry(r, true),
        "challengeDetected=true is ignored; does not retry");
    TestRunner::AssertFalse(ShouldRetry(r, false),
        "challengeDetected=false also does not retry for a clean 200");
}

TEST(web_search, ShouldRetryOnCurlError)
{
    WebSearchHttpResponse r;
    r.isCurlError = true;
    r.status = 0;
    r.err = "Couldn't resolve host";
    TestRunner::AssertTrue(ShouldRetry(r, false),
        "curl transport error retries");
    r.isCurlError = false;
    r.status = 200;
    TestRunner::AssertFalse(ShouldRetry(r, false),
        "non-error 200 with no challenge does not retry");
}

TEST(web_search, ComputeBackoffMsIncrementsExponentially)
{
    TestRunner::AssertEq(ComputeBackoffMs(0), 1000, "attempt 0 -> 1s");
    TestRunner::AssertEq(ComputeBackoffMs(1), 2000, "attempt 1 -> 2s");
    TestRunner::AssertEq(ComputeBackoffMs(2), 4000, "attempt 2 -> 4s");
    TestRunner::AssertEq(ComputeBackoffMs(3), 8000, "attempt 3 -> 8s");
    TestRunner::AssertEq(ComputeBackoffMs(-1), 0, "negative attempt is a no-op");
    // Cap check: attempt 100 should not overflow
    int huge = ComputeBackoffMs(100);
    TestRunner::AssertTrue(huge > 0, "huge attempt is capped, not overflowed");
}

TEST(web_search, PickUserAgentStableAcrossCalls)
{
    // Per-process stable: same UA returned on every call. Avoids the
    // suspicious "same IP cycling UAs between calls" pattern.
    std::string ua1 = PickUserAgent();
    std::string ua2 = PickUserAgent();
    TestRunner::AssertEq(ua1, ua2,
        "per-process UA must be stable across calls");
    TestRunner::AssertTrue(ua1.find("Mozilla/") == 0,
        "UA pool entries start with Mozilla/");
}

// ---- Result cache tests (Layer 4) ----
//
// Uses a 1-second TTL to verify expiry without sleeping for 5 minutes. The
// cache is process-global (singleton), so each test clears it first to
// avoid cross-test contamination.

TEST(web_search, ResultCacheHitAfterStore)
{
    WebSearchResultCache::Instance().Clear();
    std::vector<WebSearchResult> stored;
    WebSearchResult r;
    r.title = "Cached Title";
    r.url = "https://example.com/cached";
    r.snippet = "Cached snippet";
    stored.push_back(r);
    WebSearchResultCache::Instance().Store("duckduckgo", "test query", stored, 300);

    const auto* hit = WebSearchResultCache::Instance().Lookup("duckduckgo", "test query");
    TestRunner::AssertTrue(hit != nullptr, "cache hit after store");
    if (hit) {
        TestRunner::AssertEq(hit->size(), static_cast<size_t>(1), "one cached row");
        TestRunner::AssertEq((*hit)[0].title, std::string("Cached Title"),
            "cached row title preserved");
    }
}

TEST(web_search, ResultCacheMissOnUnknownKey)
{
    WebSearchResultCache::Instance().Clear();
    WebSearchResultCache::Instance().Store("duckduckgo", "real query", {}, 300);
    const auto* hit = WebSearchResultCache::Instance().Lookup("duckduckgo", "different query");
    TestRunner::AssertTrue(hit == nullptr, "miss on unknown query");
    // Different engine, same query — also a miss.
    hit = WebSearchResultCache::Instance().Lookup("wikipedia", "real query");
    TestRunner::AssertTrue(hit == nullptr,
        "miss on same query but different engine (cache is keyed by engine|query)");
}

TEST(web_search, ResultCacheExpiresAfterTtl)
{
    WebSearchResultCache::Instance().Clear();
    std::vector<WebSearchResult> stored;
    WebSearchResult r; r.title = "Temp";
    stored.push_back(r);
    // 1-second TTL — short enough to test expiry without sleeping 5 min.
    WebSearchResultCache::Instance().Store("bing", "expiry test", stored, 1);
    const auto* hit0 = WebSearchResultCache::Instance().Lookup("bing", "expiry test");
    TestRunner::AssertTrue(hit0 != nullptr, "hit immediately after store");
    // Sleep 1.2s to push past the TTL.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const auto* hit1 = WebSearchResultCache::Instance().Lookup("bing", "expiry test");
    TestRunner::AssertTrue(hit1 == nullptr,
        "miss after TTL expired (entry lazily evicted on lookup)");
}

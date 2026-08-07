#ifndef SRC_UTILS_CURL_CLIENT_H_
#define SRC_UTILS_CURL_CLIENT_H_

#include <curl/curl.h>

#include <functional>
#include <string>
#include <vector>

namespace jiuwen {

// Request parameters. All curl_easy_setopt options are expressed through this
// struct so callers never touch the curl API directly. body semantics:
//   - body non-empty  -> POST (CURLOPT_POST + POSTFIELDS)
//   - body empty      -> GET (CURLOPT_POST left unset)
struct CurlRequest {
    std::string url;
    std::vector<std::string> headers;  // "Name: value" form, built into curl_slist
    std::string body;                  // POST body; empty = GET
    long connectTimeout{0};            // CURLOPT_CONNECTTIMEOUT; 0 = leave default
    long requestTimeout{0};            // CURLOPT_TIMEOUT; 0 = leave default
    bool followLocation{false};        // CURLOPT_FOLLOWLOCATION
    bool sslVerify{true};              // CURLOPT_SSL_VERIFYPEER / VERIFYHOST
    std::string userAgent;             // CURLOPT_USERAGENT; empty = don't set
    std::string referer;               // CURLOPT_REFERER; empty = don't set
    // When true, enables curl's in-memory cookie engine via
    // CURLOPT_COOKIEFILE="" (load no file, start an in-memory jar) and
    // leaves CURLOPT_COOKIEJAR unset (don't persist to disk). Cookies then
    // survive across requests on the same thread_local handle (matches
    // ThreadCurlHandle's per-thread reuse model) — useful for search
    // engines that set a session cookie on the first challenge response
    // and accept it on subsequent requests. No disk I/O, no cleanup.
    bool enableCookies{false};
    // CURLOPT_NOPROXY. When non-empty, curl bypasses HTTPS_PROXY/HTTP_PROXY
    // env vars for hosts matching the listed domains. Comma-separated,
    // e.g. "baidu.com,sogou.com" or "*" for all hosts. Used by WebSearchTool
    // to route Chinese search engines through the local network (China IP)
    // directly instead of through a VPN proxy — the VPN exit IP is shared
    // with other users and gets rate-limited/flagged by Western engines,
    // while Chinese engines are reachable from the China IP without a proxy
    // and have better coverage for Chinese queries anyway.
    std::string noProxyHosts;
};

// Response result. curlCode is the raw transport-level code; retry
// classification (IsRetryableCurlError) stays with the caller. curlErrorStr
// is the human-readable message (curl_easy_strerror) so callers don't need
// to touch the curl API themselves.
struct CurlResponse {
    long statusCode{0};
    std::string body;                  // full body in non-streaming mode; empty in streaming
    CURLcode curlCode{CURLE_OK};
    bool isCurlError{false};           // curlCode != CURLE_OK
    std::string curlErrorStr;          // filled when isCurlError; empty otherwise
};

// Unified HTTP transport. Static methods, all callers share a per-thread
// persistent CURL handle (thread_local + curl_easy_reset) so consecutive
// requests on the same thread reuse TCP / TLS session / DNS cache.
//
// Lifecycle: GlobalInit() must run on the main thread before any request and
// before worker threads start; GlobalCleanup() must run after all curl-using
// threads have been joined (so their thread_local handles are destroyed).
// SessionManager wires both into InitSessionManager / Shutdown.
class CurlClient {
public:
    // Non-streaming requests.
    static CurlResponse Post(const CurlRequest& req);
    static CurlResponse Get(const CurlRequest& req);

    // Streaming requests. onChunk receives raw response bytes; the SSE line
    // parsing stays with the caller. Return false from onChunk to ABORT the
    // transfer mid-stream (curl_easy_perform returns CURLE_WRITE_ERROR). This
    // is the hook used by Model streaming to implement mid-stream cancel.
    static CurlResponse PostStream(const CurlRequest& req,
                                   std::function<bool(const char*, size_t)> onChunk);
    static CurlResponse GetStream(const CurlRequest& req,
                                  std::function<bool(const char*, size_t)> onChunk);

    // URL encode/decode via curl_easy_escape/unescape on the thread_local
    // handle (no ephemeral handle per call).
    static std::string UrlEncode(const std::string& value);
    static std::string UrlDecode(const std::string& value);

    // Global lifecycle. Both idempotent (call_once). GlobalInit also registers
    // an atexit(GlobalCleanup) fallback so cleanup still runs if Shutdown is
    // never called.
    static void GlobalInit();
    static void GlobalCleanup();
};

}  // namespace jiuwen

#endif  // SRC_UTILS_CURL_CLIENT_H_

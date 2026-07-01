#include "src/utils/curl_client.h"

#include <mutex>

#include "src/utils/logger.h"

namespace jiuwen {

namespace {

// Per-thread persistent CURL handle. First Get() lazily calls curl_easy_init();
// Reset() clears options between requests while preserving the connection
// cache, DNS cache and SSL session cache. The handle is cleaned up when the
// thread exits (reverse of how it was created). curl_global_init must have
// run before the first Get() on any thread — guaranteed by SessionManager
// wiring GlobalInit into InitSessionManager on the main thread before worker
// threads start.
class ThreadCurlHandle {
public:
    CURL* Get()
    {
        if (!handle_) {
            handle_ = curl_easy_init();
        }
        return handle_;
    }

    void Reset()
    {
        if (handle_) {
            curl_easy_reset(handle_);
        }
    }

    ~ThreadCurlHandle()
    {
        if (handle_) {
            curl_easy_cleanup(handle_);
            handle_ = nullptr;
        }
    }

private:
    CURL* handle_{nullptr};
};

thread_local ThreadCurlHandle tlHandle;

std::once_flag g_initOnce;
std::once_flag g_cleanupOnce;

// RAII wrapper for a curl_slist built from CurlRequest::headers.
struct SlistHolder {
    struct curl_slist* list{nullptr};
    explicit SlistHolder(const std::vector<std::string>& headers)
    {
        for (const auto& h : headers) {
            struct curl_slist* next = curl_slist_append(list, h.c_str());
            if (!next) {
                // allocation failure: keep partial list, will be freed in dtor
                break;
            }
            list = next;
        }
    }
    ~SlistHolder()
    {
        if (list) {
            curl_slist_free_all(list);
        }
    }
    struct curl_slist* Get() const { return list; }
};

// Write callback that appends raw bytes to a std::string.
size_t AppendWriteCallback(void* contents, size_t size, size_t nmemb, void* userdata)
{
    size_t total = size * nmemb;
    auto* out = static_cast<std::string*>(userdata);
    if (out && total) {
        out->append(static_cast<const char*>(contents), total);
    }
    return total;
}

// Write callback that dispatches raw bytes to a bool-returning onChunk.
// Returning a value != total aborts the transfer (curl_easy_perform returns
// CURLE_WRITE_ERROR), which is the mid-stream cancel hook.
struct ChunkCtx {
    std::function<bool(const char*, size_t)> onChunk;
};

size_t ChunkWriteCallback(void* contents, size_t size, size_t nmemb, void* userdata)
{
    size_t total = size * nmemb;
    auto* ctx = static_cast<ChunkCtx*>(userdata);
    if (!ctx || !ctx->onChunk) {
        return total;  // no callback: accept and discard
    }
    if (total == 0) {
        return 0;
    }
    bool keep = ctx->onChunk(static_cast<const char*>(contents), total);
    return keep ? total : 0;  // 0 signals "abort" to curl
}

// Shared option setup for all request modes. Sets everything except
// WRITEFUNCTION / WRITEDATA, which differ between string-append and streaming.
// Sets POST + POSTFIELDS when req.body is non-empty, leaves GET default otherwise.
void ApplyOptions(CURL* h, const CurlRequest& req, struct curl_slist* headerList)
{
    curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
    if (headerList) {
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, headerList);
    }
    if (!req.body.empty()) {
        curl_easy_setopt(h, CURLOPT_POST, 1L);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    }
    if (req.connectTimeout > 0) {
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, req.connectTimeout);
    }
    if (req.requestTimeout > 0) {
        curl_easy_setopt(h, CURLOPT_TIMEOUT, req.requestTimeout);
    }
    if (req.followLocation) {
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    }
    if (!req.sslVerify) {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (!req.userAgent.empty()) {
        curl_easy_setopt(h, CURLOPT_USERAGENT, req.userAgent.c_str());
    }
}

// Core perform routine shared by Post/Get/PostStream/GetStream. When onChunk
// is empty, response body is accumulated into resp.body; otherwise raw bytes
// are dispatched to onChunk (and resp.body stays empty).
CurlResponse Perform(const CurlRequest& req, std::function<bool(const char*, size_t)> onChunk)
{
    CurlResponse resp;
    CURL* h = tlHandle.Get();
    if (!h) {
        resp.curlCode = CURLE_FAILED_INIT;
        resp.isCurlError = true;
        return resp;
    }
    tlHandle.Reset();

    SlistHolder slist(req.headers);
    ApplyOptions(h, req, slist.Get());

    std::string bodyBuffer;
    ChunkCtx chunkCtx;
    if (onChunk) {
        chunkCtx.onChunk = std::move(onChunk);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, ChunkWriteCallback);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &chunkCtx);
    } else {
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, AppendWriteCallback);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &bodyBuffer);
    }

    CURLcode rc = curl_easy_perform(h);
    resp.curlCode = rc;
    resp.isCurlError = (rc != CURLE_OK);
    if (rc != CURLE_OK) {
        resp.curlErrorStr = curl_easy_strerror(rc);
    }

    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    resp.statusCode = code;

    if (!onChunk) {
        resp.body = std::move(bodyBuffer);
    }

    if (rc != CURLE_OK) {
        LOG(WARN) << "[CurlClient] perform failed: " << resp.curlErrorStr
                  << " url=" << req.url << " httpCode=" << code;
    }
    return resp;
}

}  // namespace

CurlResponse CurlClient::Post(const CurlRequest& req)
{
    return Perform(req, {});
}

CurlResponse CurlClient::Get(const CurlRequest& req)
{
    return Perform(req, {});
}

CurlResponse CurlClient::PostStream(const CurlRequest& req,
                                    std::function<bool(const char*, size_t)> onChunk)
{
    return Perform(req, std::move(onChunk));
}

CurlResponse CurlClient::GetStream(const CurlRequest& req,
                                   std::function<bool(const char*, size_t)> onChunk)
{
    return Perform(req, std::move(onChunk));
}

std::string CurlClient::UrlEncode(const std::string& value)
{
    CURL* h = tlHandle.Get();
    if (!h) {
        return value;
    }
    tlHandle.Reset();
    char* encoded = curl_easy_escape(h, value.c_str(), static_cast<int>(value.size()));
    std::string result = encoded ? encoded : value;
    if (encoded) {
        curl_free(encoded);
    }
    return result;
}

std::string CurlClient::UrlDecode(const std::string& value)
{
    CURL* h = tlHandle.Get();
    if (!h) {
        return value;
    }
    tlHandle.Reset();
    int outLen = 0;
    char* decoded = curl_easy_unescape(h, value.c_str(), static_cast<int>(value.size()), &outLen);
    std::string result = decoded ? std::string(decoded, static_cast<size_t>(outLen)) : value;
    if (decoded) {
        curl_free(decoded);
    }
    return result;
}

void CurlClient::GlobalInit()
{
    std::call_once(g_initOnce, []() {
        CURLcode rc = curl_global_init(CURL_GLOBAL_ALL);
        if (rc != CURLE_OK) {
            LOG(ERR) << "[CurlClient] curl_global_init failed: " << curl_easy_strerror(rc);
            return;
        }
        // Fallback cleanup if Shutdown() is never called. Idempotent via
        // g_cleanupOnce, so an explicit GlobalCleanup() later is a no-op.
        std::atexit(&CurlClient::GlobalCleanup);
        LOG(INFO) << "[CurlClient] curl_global_init ok";
    });
}

void CurlClient::GlobalCleanup()
{
    std::call_once(g_cleanupOnce, []() {
        curl_global_cleanup();
        LOG(INFO) << "[CurlClient] curl_global_cleanup ok";
    });
}

}  // namespace jiuwen

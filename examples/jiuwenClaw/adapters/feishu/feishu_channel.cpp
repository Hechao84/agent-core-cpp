#define CPPHTTPLIB_KEEPALIVE_TIMEOUT_SECOND 10
#define CPPHTTPLIB_KEEPALIVE_MAX_COUNT 5
#include "examples/jiuwenClaw/adapters/feishu/feishu_channel.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "examples/jiuwenClaw/utils/logger.h"

#include "curl/curl.h"
#include "httplib.h"
#include "nlohmann/json.hpp"

namespace jiuwenClaw {

// ============================================================
// pbbp2 Protobuf Frame encoder/decoder (minimal varint impl)
// ============================================================
// Protobuf wire format (proto2):
//   varint = uint64/uint32, length-delimited = string/bytes/embedded_msg
// Frame proto:
//   Header { string key=1; string value=2; }
//   Frame { uint64 SeqID=1; uint64 LogID=2; int32 service=3;
//           int32 method=4; repeated Header headers=5;
//           string payload_encoding=6; string payload_type=7;
//           bytes payload=8; string LogIDNew=9; }

namespace pb {

static void AppendVarint(std::string& out, uint64_t value)
{
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

static uint64_t ReadVarint(const std::string& in, size_t& pos)
{
    uint64_t result = 0;
    int shift = 0;
    while (pos < in.size() && shift < 64) {
        unsigned char b = static_cast<unsigned char>(in[pos++]);
        result |= (static_cast<uint64_t>(b & 0x7F) << shift);
        if (!(b & 0x80)) {
            return result;
        }
        shift += 7;
    }
    return result;
}

static std::string ReadLengthDelimited(const std::string& in, size_t& pos)
{
    uint64_t len = ReadVarint(in, pos);
    if (pos + len > in.size()) {
        len = in.size() - pos;
    }
    std::string result = in.substr(pos, static_cast<size_t>(len));
    pos += static_cast<size_t>(len);
    return result;
}

struct Header
{
    std::string key;
    std::string value;
};

struct Frame
{
    uint64_t seqId = 0;
    uint64_t logId = 0;
    int32_t service = 0;
    int32_t method = 0;       // 0=CONTROL, 1=DATA
    std::vector<Header> headers;
    std::string payload;
    std::string payloadType;  // "ping", "pong", "event", "card"
};

static std::string EncodeFrame(const Frame& frame)
{
    std::string out;
    AppendVarint(out, (1 << 3) | 0);  // field 1, wire_type 0
    AppendVarint(out, frame.seqId);
    AppendVarint(out, (2 << 3) | 0);  // field 2
    AppendVarint(out, frame.logId);
    AppendVarint(out, (3 << 3) | 0);  // field 3
    AppendVarint(out, static_cast<uint64_t>(frame.service));
    AppendVarint(out, (4 << 3) | 0);  // field 4
    AppendVarint(out, static_cast<uint64_t>(frame.method));

    for (const auto& h : frame.headers) {
        std::string hdr;
        AppendVarint(hdr, (1 << 3) | 2);
        AppendVarint(hdr, h.key.size());
        hdr.append(h.key);
        AppendVarint(hdr, (2 << 3) | 2);
        AppendVarint(hdr, h.value.size());
        hdr.append(h.value);
        AppendVarint(out, (5 << 3) | 2);
        AppendVarint(out, hdr.size());
        out.append(hdr);
    }

    if (!frame.payload.empty()) {
        AppendVarint(out, (8 << 3) | 2);
        AppendVarint(out, frame.payload.size());
        out.append(frame.payload);
    }
    return out;
}

static Frame DecodeFrame(const std::string& in)
{
    Frame frame;
    size_t pos = 0;
    std::map<std::string, std::string> headerMap;

    while (pos < in.size()) {
        uint64_t tag = ReadVarint(in, pos);
        uint32_t field = static_cast<uint32_t>(tag >> 3);
        uint32_t wireType = static_cast<uint32_t>(tag & 0x07);

        if (wireType == 0) {
            uint64_t val = ReadVarint(in, pos);
            switch (field) {
            case 1:
                frame.seqId = val;
                break;
            case 2:
                frame.logId = val;
                break;
            case 3:
                frame.service = static_cast<int32_t>(val);
                break;
            case 4:
                frame.method = static_cast<int32_t>(val);
                break;
            }
        } else if (wireType == 2) {
            std::string data = ReadLengthDelimited(in, pos);
            switch (field) {
            case 5: {
                size_t hpos = 0;
                Header h;
                // Parse Header sub-message
                while (hpos < data.size()) {
                    uint64_t htag = ReadVarint(data, hpos);
                    int hfield = static_cast<int>(htag >> 3);
                    int hwire = static_cast<int>(htag & 0x07);
                    if (hwire == 2) {
                        std::string sval =
                            ReadLengthDelimited(data, hpos);
                        if (hfield == 1)
                            h.key = sval;
                        else if (hfield == 2)
                            h.value = sval;
                    } else if (hwire == 0) {
                        ReadVarint(data, hpos);
                    }
                }
                if (!h.key.empty()) {
                    frame.headers.push_back(h);
                    headerMap[h.key] = h.value;
                }
                break;
            }
            case 6:
                frame.payloadType = data;
                break;
            case 7:
                frame.payloadType = data;
                break;
            case 8:
                frame.payload = data;
                break;
            case 9:
                break;
            default:
                break;
            }
        }
    }

    // Extract payloadType from headers
    for (const auto& h : frame.headers) {
        if (h.key == "type")
            frame.payloadType = h.value;
    }

    return frame;
}

static Frame MakePingFrame(int32_t serviceId)
{
    Frame frame;
    frame.seqId = 0;
    frame.logId = 0;
    frame.service = serviceId;
    frame.method = 0; // CONTROL
    frame.headers.push_back({"type", "ping"});
    return frame;
}

} // namespace pb

// ============================================================
// Feishu Channel Implementation
// ============================================================

struct FeishuChannel::Impl
{
    FeishuConfig config;
    std::string cachedToken_;
    std::chrono::steady_clock::time_point tokenExpiresAt_;
    std::mutex tokenMutex_;

    // WebSocket connection
    std::unique_ptr<httplib::ws::WebSocketClient> ws_;
    std::string wsUrl_;
    int32_t serviceId_ = 0;
    int pingInterval_ = 120; // seconds
    std::chrono::steady_clock::time_point lastPing_;

    FeishuChannel::EventCallback eventCallback_;

    bool EnsureToken();
    std::string HttpPost(const std::string& url, const std::string& body);
    bool DiscoverEndpoint();
    bool ConnectWebSocket();
    void DisconnectWebSocket();
    void RunReadLoop();
    void HandleFrame(const std::string& rawData);
    void HandleControlFrame(const pb::Frame& frame);
    void HandleDataFrame(const pb::Frame& frame);
    void SendEventAck(const pb::Frame& eventFrame);
    void DispatchEvent(const std::string& eventPayload);
    bool SendFrame(const pb::Frame& frame);
    bool SendTextMessageToFeishu(const std::string& chatId,
                                 const std::string& text,
                                 std::string* outMsgId = nullptr);
    bool UpdateTextMessage(const std::string& messageId,
                           const std::string& text);
};

static size_t WriteCallback(void* contents, size_t size, size_t nmemb,
                            std::string* output)
{
    size_t total = size * nmemb;
    output->append(static_cast<char*>(contents), total);
    return total;
}

std::string FeishuChannel::Impl::HttpPost(const std::string& url,
                                          const std::string& body)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG(ERR) << "[FeishuChannel] Failed to init CURL";
        return "";
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(
        headers, "Content-Type: application/json; charset=utf-8");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);

    std::string response;
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        LOG(ERR) << "[FeishuChannel] HTTP POST failed: "
                 << curl_easy_strerror(res);
        response.clear();
    } else {
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        LOG(DBG) << "[FeishuChannel] HTTP POST " << url << " -> " << httpCode;
    }

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return response;
}

bool FeishuChannel::Impl::EnsureToken()
{
    std::lock_guard<std::mutex> lock(tokenMutex_);
    auto now = std::chrono::steady_clock::now();
    if (!cachedToken_.empty() && now < tokenExpiresAt_)
        return true;

    LOG(INFO) << "[FeishuChannel] Refreshing tenant_access_token...";

    nlohmann::json req;
    req["app_id"] = config.appId;
    req["app_secret"] = config.appSecret;

    std::string response = HttpPost(
        "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal",
        req.dump());

    if (response.empty()) {
        LOG(ERR) << "[FeishuChannel] Token request: empty response";
        return false;
    }

    try {
        auto j = nlohmann::json::parse(response);
        int code = j.value("code", -1);
        if (code != 0) {
            LOG(ERR) << "[FeishuChannel] Token error: code=" << code
                     << ", msg=" << j.value("msg", "");
            return false;
        }
        cachedToken_ = j.value("tenant_access_token", "");
        int expire = j.value("expire", 7200);
        tokenExpiresAt_ = now + std::chrono::seconds(expire - 300);
        LOG(INFO) << "[FeishuChannel] Token refreshed, expires in " << expire
                  << "s";
        return true;
    } catch (const std::exception& e) {
        LOG(ERR) << "[FeishuChannel] Parse token response: " << e.what();
        return false;
    }
}

bool FeishuChannel::Impl::DiscoverEndpoint()
{
    nlohmann::json req;
    req["AppID"] = config.appId;
    req["AppSecret"] = config.appSecret;

    std::string response = HttpPost(
        "https://open.feishu.cn/callback/ws/endpoint", req.dump());

    if (response.empty()) {
        LOG(ERR) << "[FeishuChannel] Endpoint discovery: empty response";
        return false;
    }

    try {
        auto j = nlohmann::json::parse(response);
        int code = j.value("code", -1);
        if (code != 0) {
            LOG(ERR) << "[FeishuChannel] Endpoint error: code=" << code
                     << ", msg=" << j.value("msg", "");
            return false;
        }

        auto data = j.value("data", nlohmann::json::object());
        wsUrl_ = data.value("URL", "");
        if (wsUrl_.empty()) {
            LOG(ERR) << "[FeishuChannel] Endpoint: no URL returned";
            return false;
        }

        // Parse service_id and ping interval from ClientConfig
        auto cc = data.value("ClientConfig", nlohmann::json::object());
        pingInterval_ = cc.value("PingInterval", 120);

        // Extract service_id from URL query string
        auto qpos = wsUrl_.find("service_id=");
        if (qpos != std::string::npos) {
            auto valStart = qpos + 11;
            auto valEnd = wsUrl_.find('&', valStart);
            if (valEnd == std::string::npos)
                valEnd = wsUrl_.size();
            std::string sid =
                wsUrl_.substr(valStart, valEnd - valStart);
            try {
                serviceId_ = std::stoi(sid);
            } catch (...) {
                serviceId_ = 1;
            }
        }

        LOG(INFO) << "[FeishuChannel] Endpoint: URL=" << wsUrl_
                  << ", serviceId=" << serviceId_
                  << ", pingInterval=" << pingInterval_ << "s";
        return true;
    } catch (const std::exception& e) {
        LOG(ERR) << "[FeishuChannel] Parse endpoint response: " << e.what();
        return false;
    }
}

bool FeishuChannel::Impl::ConnectWebSocket()
{
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    ws_ = std::make_unique<httplib::ws::WebSocketClient>(wsUrl_);
    ws_->enable_server_certificate_verification(false); // TODO: proper CA setup
#else
    LOG(ERR) << "[FeishuChannel] wss:// not supported. Compile with "
             << "CPPHTTPLIB_OPENSSL_SUPPORT defined.";
    return false;
#endif

    if (!ws_->is_valid()) {
        LOG(ERR) << "[FeishuChannel] WebSocketClient URL parse failed: " << wsUrl_;
        return false;
    }

    LOG(INFO) << "[FeishuChannel] WebSocketClient is_valid: true";

    ws_->set_read_timeout(60, 0);
    ws_->set_write_timeout(10, 0);
    ws_->set_websocket_ping_interval(0);

    LOG(INFO) << "[FeishuChannel] Connecting WebSocket...";
    if (!ws_->connect()) {
        LOG(ERR) << "[FeishuChannel] WebSocket connect() returned false";
        LOG(ERR) << "[FeishuChannel] This may be caused by:";
        LOG(ERR) << "[FeishuChannel]   1. TLS certificate verification failed";
        LOG(ERR) << "[FeishuChannel]   2. Network cannot reach msg-frontier.feishu.cn";
        LOG(ERR) << "[FeishuChannel]   3. CPPHTTPLIB_OPENSSL_SUPPORT not active";
        return false;
    }

    LOG(INFO) << "[FeishuChannel] WebSocket connected";
    lastPing_ = std::chrono::steady_clock::now();
    return true;
}

void FeishuChannel::Impl::DisconnectWebSocket()
{
    if (ws_) {
        ws_->close(httplib::ws::CloseStatus::Normal, "bye");
        ws_.reset();
        LOG(INFO) << "[FeishuChannel] WebSocket disconnected";
    }
}

bool FeishuChannel::Impl::SendFrame(const pb::Frame& frame)
{
    if (!ws_ || !ws_->is_open())
        return false;

    std::string data = pb::EncodeFrame(frame);
    return ws_->send(data.data(), data.size());
}

void FeishuChannel::Impl::HandleFrame(const std::string& rawData)
{
    pb::Frame frame = pb::DecodeFrame(rawData);

    if (frame.method == 0) { // CONTROL
        HandleControlFrame(frame);
    } else if (frame.method == 1) { // DATA
        HandleDataFrame(frame);
    }
}

void FeishuChannel::Impl::HandleControlFrame(const pb::Frame& frame)
{
    if (frame.payloadType == "ping") {
        pb::Frame pong;
        pong.seqId = 0;
        pong.logId = frame.logId;
        pong.service = frame.service;
        pong.method = 0;
        pong.headers.push_back({"type", "pong"});
        pong.payload = frame.payload;
        if (!SendFrame(pong)) {
            LOG(WARN) << "[FeishuChannel] Failed to send pong";
        } else {
            LOG(DBG) << "[FeishuChannel] Pong sent";
        }
        return;
    }

    if (frame.payloadType == "pong") {
        lastPing_ = std::chrono::steady_clock::now();
        LOG(DBG) << "[FeishuChannel] Pong received";
    }
}

void FeishuChannel::Impl::HandleDataFrame(const pb::Frame& frame)
{
    if (frame.payloadType != "event") {
        return;
    }

    LOG(INFO) << "[FeishuChannel] Event received, seqId=" << frame.seqId
              << ", service=" << frame.service
              << ", logId=" << frame.logId
              << ", payloadSize=" << frame.payload.size()
              << ", headers=" << frame.headers.size();

    SendEventAck(frame);
    DispatchEvent(frame.payload);
}

void FeishuChannel::Impl::SendEventAck(const pb::Frame& eventFrame)
{
    auto ackStart = std::chrono::steady_clock::now();
    pb::Frame ackFrame;
    ackFrame.seqId = eventFrame.seqId;
    ackFrame.logId = eventFrame.logId;
    ackFrame.service = eventFrame.service;
    ackFrame.method = eventFrame.method;
    ackFrame.headers = eventFrame.headers;

    auto ackEnd = std::chrono::steady_clock::now();
    auto bizRtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                       ackEnd - ackStart).count();
    ackFrame.headers.push_back({"biz_rt", std::to_string(bizRtMs)});

    nlohmann::json respJson;
    respJson["code"] = 200;
    ackFrame.payload = respJson.dump();

    std::string ackData = pb::EncodeFrame(ackFrame);
    LOG(INFO) << "[FeishuChannel] Sending ACK: seqId=" << eventFrame.seqId
              << ", encSize=" << ackData.size()
              << ", headers=" << ackFrame.headers.size();
    bool ackOk = ws_->send(ackData.data(), ackData.size());
    LOG(INFO) << "[FeishuChannel] ACK sent ok=" << ackOk;
}

void FeishuChannel::Impl::DispatchEvent(const std::string& eventPayload)
{
    try {
        nlohmann::json event = nlohmann::json::parse(eventPayload);
        std::string schema = event.value("schema", "");
        if (schema != "2.0") {
            return;
        }

        auto ev = event.value("event", nlohmann::json::object());
        auto msg = ev.value("message", nlohmann::json::object());
        auto sender = ev.value("sender", nlohmann::json::object());

        std::string chatId = msg.value("chat_id", "");
        std::string msgType = msg.value("message_type", "");
        std::string contentStr = msg.value("content", "");
        std::string senderId;

        auto sidObj = sender.value("sender_id", nlohmann::json::object());
        senderId = sidObj.value("open_id", "");

        if (msgType != "text") {
            LOG(DBG) << "[FeishuChannel] Non-text event: " << msgType;
            return;
        }

        std::string textContent;
        try {
            auto c = nlohmann::json::parse(contentStr);
            textContent = c.value("text", "");
        } catch (...) {
            textContent = contentStr;
        }

        if (textContent.empty()) {
            return;
        }

        LOG(INFO) << "[FeishuChannel] Received from [" << chatId << "]: " << textContent;

        if (eventCallback_) {
            eventCallback_(chatId, msg.value("message_id", ""), senderId, textContent);
        }
    } catch (const std::exception& e) {
        LOG(ERR) << "[FeishuChannel] Parse event: " << e.what();
    }
}

void FeishuChannel::Impl::RunReadLoop()
{
    LOG(INFO) << "[FeishuChannel] Read loop started";

    while (true) {
        std::string msg;
        auto result = ws_->read(msg);

        if (result == httplib::ws::ReadResult::Fail) {
            LOG(WARN) << "[FeishuChannel] WebSocket read result="
                      << static_cast<int>(result);
            break;
        }

        if (result == httplib::ws::ReadResult::Binary) {
            HandleFrame(msg);
        } else if (result == httplib::ws::ReadResult::Text) {
            LOG(DBG) << "[FeishuChannel] Text frame: "
                     << msg.substr(0, 100);
        }

        // Send ping if interval elapsed
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                           now - lastPing_)
                           .count();
        if (elapsed >= pingInterval_) {
            pb::Frame ping = pb::MakePingFrame(serviceId_);
            SendFrame(ping);
            lastPing_ = now;
            LOG(DBG) << "[FeishuChannel] Ping sent";
        }
    }

    LOG(INFO) << "[FeishuChannel] Read loop exited";
}

bool FeishuChannel::Impl::SendTextMessageToFeishu(const std::string& chatId,
                                                  const std::string& text,
                                                  std::string* outMsgId)
{
    if (!EnsureToken())
        return false;

    nlohmann::json req;
    req["receive_id"] = chatId;
    req["msg_type"] = "text";
    nlohmann::json content;
    content["text"] = text;
    req["content"] = content.dump();

    std::string url =
        "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=chat_id";

    CURL* curl = curl_easy_init();
    if (!curl)
        return false;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth = "Authorization: Bearer " + cachedToken_;
    headers = curl_slist_append(headers, auth.c_str());

    std::string body = req.dump();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);

    std::string response;
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        LOG(ERR) << "[FeishuChannel] Send message failed: "
                 << curl_easy_strerror(res);
        return false;
    }

    try {
        auto j = nlohmann::json::parse(response);
        int code = j.value("code", -1);
        if (code != 0) {
            LOG(ERR) << "[FeishuChannel] Send failed: code=" << code
                     << ", msg=" << j.value("msg", "");
            return false;
        }
        if (outMsgId) {
            auto data = j.value("data", nlohmann::json::object());
            *outMsgId = data.value("message_id", "");
        }
        return true;
    } catch (const std::exception& e) {
        LOG(ERR) << "[FeishuChannel] Parse send response: " << e.what();
        return false;
    }
}

bool FeishuChannel::Impl::UpdateTextMessage(const std::string& messageId,
                                            const std::string& text)
{
    if (messageId.empty() || !EnsureToken())
        return false;

    nlohmann::json req;
    req["msg_type"] = "text";
    nlohmann::json content;
    content["text"] = text;
    req["content"] = content.dump();

    std::string url =
        "https://open.feishu.cn/open-apis/im/v1/messages/" + messageId;

    CURL* curl = curl_easy_init();
    if (!curl)
        return false;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth = "Authorization: Bearer " + cachedToken_;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "X-HTTP-Method-Override: PUT");

    std::string body = req.dump();
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        LOG(ERR) << "[FeishuChannel] Update message failed: "
                 << curl_easy_strerror(res);
        return false;
    }

    try {
        auto j = nlohmann::json::parse(response);
        int code = j.value("code", -1);
        if (code != 0) {
            LOG(DBG) << "[FeishuChannel] Update failed: code=" << code
                     << ", msg=" << j.value("msg", "");
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

FeishuChannel::FeishuChannel() = default;

FeishuChannel::~FeishuChannel()
{
    Stop();
}

void FeishuChannel::Start(const FeishuConfig& config)
{
    if (config.appId.empty() || config.appSecret.empty()) {
        LOG(ERR) << "[FeishuChannel] appId or appSecret is empty";
        return;
    }

    LOG(INFO) << "[FeishuChannel] Starting (long connection mode) "
              << config.appId;

    impl_ = std::make_unique<Impl>();
    impl_->config = config;
    if (eventCallback_)
        impl_->eventCallback_ = std::move(eventCallback_);

    // Discover endpoint
    if (!impl_->DiscoverEndpoint()) {
        LOG(ERR) << "[FeishuChannel] Endpoint discovery failed";
        return;
    }

    // Connect WebSocket
    if (!impl_->ConnectWebSocket()) {
        LOG(ERR) << "[FeishuChannel] WebSocket connect failed";
        return;
    }

    running_ = true;
    connThread_ = std::thread([this]() { impl_->RunReadLoop(); });

    LOG(INFO) << "[FeishuChannel] Started successfully";
}

void FeishuChannel::Stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_)
        return;

    running_ = false;

    if (impl_) {
        impl_->DisconnectWebSocket();
        impl_.reset();
    }

    if (connThread_.joinable())
        connThread_.join();

    LOG(INFO) << "[FeishuChannel] Stopped";
}

bool FeishuChannel::IsRunning() const
{
    return running_;
}

void FeishuChannel::SetEventCallback(EventCallback callback)
{
    eventCallback_ = std::move(callback);
}

bool FeishuChannel::SendTextMessage(const std::string& chatId,
                                    const std::string& text)
{
    if (!impl_)
        return false;
    return impl_->SendTextMessageToFeishu(chatId, text);
}

} // namespace jiuwenClaw

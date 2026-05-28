#define CPPHTTPLIB_KEEPALIVE_TIMEOUT_SECOND 10
#define CPPHTTPLIB_KEEPALIVE_MAX_COUNT 5
#include "httplib.h"

#include "src/web/web_api.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "src/channels/channel_manager.h"
#include "src/channels/feishu_channel.h"
#include "src/context_engine/context_engine.h"
#include "src/utils/logger.h"

#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

namespace {

// Window during which the same Feishu event_id should be ignored as a redelivery.
constexpr auto kFeishuDedupWindow = std::chrono::minutes(5);

// Stream-callback tags emitted by the agent loop.
constexpr const char* kTagToolCalls = "[TOOL_CALLS]";
constexpr const char* kTagToolResponse = "[TOOL_RESPONSE]";
constexpr const char* kTagFinal = "[FINAL]";

constexpr const char* kStatusProcessing = "[Status] Processing...";
constexpr const char* kStatusComplete = "[Status] Complete";

// Aggregated state shared between the agent worker thread and the bubble-sender
// thread for a single Feishu inbound event.
struct FeishuTurnContext
{
    std::string finalText;
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    SessionInvokeResult result;
};

void TrimAround(std::string& s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '\r')) {
        s.erase(0, 1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
}

// Returns true and writes the trimmed remainder if `text` starts (or contains
// at offset 0..n) the tag; otherwise returns false.
bool TryExtractTag(const std::string& text, const char* tag, std::string& body)
{
    size_t pos = text.find(tag);
    if (pos == std::string::npos) {
        return false;
    }
    body = text.substr(pos + std::char_traits<char>::length(tag));
    TrimAround(body);
    return true;
}

// Returns JSON field as string. Non-string scalars are dump()'d; missing/null
// returns empty string.
std::string JsonFieldAsString(const nlohmann::json& j, const char* key)
{
    if (!j.contains(key) || j[key].is_null()) {
        return "";
    }
    if (j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return j[key].dump();
}

// Returns true if `messageId` is a fresh Feishu event (not seen within the
// dedup window). On true, the id is recorded; on false, the caller should
// skip processing because Feishu re-delivered the same event.
bool RegisterFeishuEventOnce(const std::string& messageId)
{
    using TimePoint = std::chrono::steady_clock::time_point;
    static std::mutex mutex;
    static std::deque<std::pair<std::string, TimePoint>> seen;

    std::lock_guard<std::mutex> lock(mutex);
    auto now = std::chrono::steady_clock::now();
    while (!seen.empty() && now - seen.front().second > kFeishuDedupWindow) {
        seen.pop_front();
    }
    for (const auto& entry : seen) {
        if (entry.first == messageId) {
            return false;
        }
    }
    seen.push_back({messageId, now});
    return true;
}

// Sends a single text message to Feishu as one bubble. Empty strings are
// ignored so callers can pass intermediate computed text freely.
void SendBubble(FeishuChannel* channel, const std::string& chatId, const std::string& text)
{
    if (text.empty()) {
        return;
    }
    LOG(INFO) << "[WebApi/Feishu] Bubble to chat=" << chatId
              << ", size=" << text.size()
              << ", first=" << text.substr(0, 80);
    channel->SendTextMessage(chatId, text);
}

// Renders a [TOOL_CALLS] stream chunk as a Feishu bubble.
// The chunk body is a JSON object {"name": "...", "arguments": ...}; we fall
// back to the raw body if parsing fails.
void EmitToolCallBubble(FeishuChannel* channel, const std::string& chatId,
                        const std::string& body)
{
    std::string name = body;
    std::string args;
    try {
        auto j = nlohmann::json::parse(body);
        name = JsonFieldAsString(j, "name");
        args = JsonFieldAsString(j, "arguments");
    } catch (...) {
        // Leave name as raw body, args empty.
    }
    std::string bubble = "[TOOL_CALL]" + name;
    if (!args.empty()) {
        bubble += " args=" + args;
    }
    SendBubble(channel, chatId, bubble);
}

// Routes one stream chunk from the agent loop into Feishu bubbles.
//   [TOOL_CALLS]  -> immediate [TOOL_CALL] bubble
//   [TOOL_RESPONSE] -> immediate [TOOL_RESULT] bubble (raw observation)
//   [FINAL]       -> buffered to ctx->finalText; emitted after agent ends
//   [STATUS]/[STREAM] -> dropped (only opening/closing status are shown)
void RouteAgentStreamChunk(const std::shared_ptr<FeishuTurnContext>& ctx,
                           FeishuChannel* channel, const std::string& chatId,
                           const std::string& resp)
{
    if (resp.empty()) {
        return;
    }
    std::string body;
    if (TryExtractTag(resp, kTagToolCalls, body)) {
        EmitToolCallBubble(channel, chatId, body);
        return;
    }
    if (TryExtractTag(resp, kTagToolResponse, body)) {
        SendBubble(channel, chatId, std::string("[TOOL_RESULT]\n") + body);
        return;
    }
    if (TryExtractTag(resp, kTagFinal, body)) {
        std::lock_guard<std::mutex> lk(ctx->mtx);
        ctx->finalText = body;
        ctx->cv.notify_all();
        return;
    }
    // Other tags ([STATUS]/[STREAM]) are intentionally dropped.
}

// Invokes the agent in a detached worker thread, streaming chunks through
// RouteAgentStreamChunk. Signals ctx->done on completion.
void SpawnAgentWorker(const std::shared_ptr<FeishuTurnContext>& ctx,
                      const ChannelMessage& msg,
                      std::function<void(const std::string&)> streamCb)
{
    std::thread([ctx, msg, streamCb = std::move(streamCb)]() {
        try {
            ctx->result = GetSessionManager().InvokeChannel(msg, streamCb);
            LOG(INFO) << "[WebApi/Feishu] Agent done, success=" << ctx->result.success;
        } catch (const std::exception& e) {
            LOG(ERR) << "[WebApi/Feishu] InvokeChannel: " << e.what();
            ctx->result.success = false;
            ctx->result.errorMessage = e.what();
        }
        {
            std::lock_guard<std::mutex> lk(ctx->mtx);
            ctx->done = true;
        }
        ctx->cv.notify_all();
    }).detach();
}

// Waits in a detached thread for the agent to finish, then sends the final
// answer bubble followed by the closing status bubble.
void SpawnClosingSender(const std::shared_ptr<FeishuTurnContext>& ctx,
                        FeishuChannel* channel, const std::string& chatId)
{
    std::thread([ctx, channel, chatId]() {
        std::unique_lock<std::mutex> lk(ctx->mtx);
        ctx->cv.wait(lk, [&]() { return ctx->done; });

        std::string finalText = ctx->finalText;
        if (finalText.empty()) {
            if (ctx->result.success && !ctx->result.content.empty()) {
                finalText = ctx->result.content;
            } else if (!ctx->result.errorMessage.empty()) {
                finalText = "Error: " + ctx->result.errorMessage;
            }
        }
        lk.unlock();

        SendBubble(channel, chatId, finalText);
        SendBubble(channel, chatId, kStatusComplete);
    }).detach();
}

// Top-level Feishu inbound event handler. Splits stream events into the
// bubble sequence specified by the product (Processing -> tool calls/results
// -> final answer -> Complete).
void HandleFeishuEvent(FeishuChannel* channel,
                       const std::string& chatId,
                       const std::string& messageId,
                       const std::string& senderId,
                       const std::string& msgContent)
{
    if (!RegisterFeishuEventOnce(messageId)) {
        LOG(INFO) << "[WebApi/Feishu] Duplicate event skipped: messageId=" << messageId;
        return;
    }

    LOG(INFO) << "[WebApi/Feishu] Event from chat=" << chatId
              << ", messageId=" << messageId
              << ", content=" << msgContent.substr(0, 100);

    SendBubble(channel, chatId, kStatusProcessing);

    auto ctx = std::make_shared<FeishuTurnContext>();

    auto streamCb = [ctx, channel, chatId](const std::string& resp) {
        RouteAgentStreamChunk(ctx, channel, chatId, resp);
    };

    ChannelMessage msg;
    msg.channel = "feishu";
    msg.chatId = chatId;
    msg.senderId = senderId;
    msg.content = msgContent;

    LOG(INFO) << "[WebApi/Feishu] Invoking agent...";
    SpawnAgentWorker(ctx, msg, streamCb);
    SpawnClosingSender(ctx, channel, chatId);
}

} // namespace

struct RunningChannel
{
    ChannelConfig config;
};

struct WebApi::Impl
{
    std::unique_ptr<httplib::Server> server;
    std::thread serverThread;
    WebApiConfig config;
    std::map<std::string, RunningChannel> channels;
    std::unique_ptr<FeishuChannel> feishuChannel; ///< Single Feishu long-connection channel
    std::mutex channelsMutex;

    void StartFeishu(const ChannelConfig& ch)
    {
        if (feishuChannel) {
            feishuChannel->Stop();
        }

        FeishuConfig fc;
        fc.appId = ch.params.count("appId") ? ch.params.at("appId") : "";
        fc.appSecret = ch.params.count("appSecret") ? ch.params.at("appSecret") : "";

        if (fc.appId.empty() || fc.appSecret.empty()) {
            LOG(WARN) << "[WebApi] Skipping Feishu channel " << ch.id
                      << ": appId or appSecret missing";
            return;
        }

        feishuChannel = std::make_unique<FeishuChannel>();
        FeishuChannel* channelPtr = feishuChannel.get();

        feishuChannel->SetEventCallback(
            [channelPtr](const std::string& chatId, const std::string& messageId,
                         const std::string& senderId, const std::string& msgContent) {
                HandleFeishuEvent(channelPtr, chatId, messageId, senderId, msgContent);
            });

        LOG(INFO) << "[WebApi] Starting Feishu long connection: appId=" << fc.appId;
        feishuChannel->Start(fc);
        if (feishuChannel->IsRunning()) {
            LOG(INFO) << "[WebApi] Feishu channel " << ch.id << " connected successfully";
        } else {
            LOG(ERR) << "[WebApi] Feishu channel " << ch.id << " FAILED to connect";
            feishuChannel.reset();
        }
    }
};

WebApi::WebApi() = default;

WebApi::~WebApi()
{
    Stop();
}

void WebApi::Start(const WebApiConfig& config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        LOG(WARN) << "[WebApi] Already running";
        return;
    }

    impl_ = std::make_unique<Impl>();
    impl_->config = config;
    impl_->server = std::make_unique<httplib::Server>();

    url_ = "http://" + config.host + ":" + std::to_string(config.port);

    // ============================================================
    // Load persistent channels and auto-start Feishu WebSocket
    // ============================================================
    ChannelManager::GetInstance().SetPersistPath("./data/channels.json");
    ChannelManager::GetInstance().Load();
    for (const auto& ch : ChannelManager::GetInstance().GetAllChannels()) {
        if (ch.type == "feishu" && ch.enabled) {
            impl_->StartFeishu(ch);
            break; // Only one Feishu channel for now
        }
    }

    if (config.enableCors) {
        impl_->server->set_pre_routing_handler(
            [](const httplib::Request& req, httplib::Response& res) {
                (void)req;
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
                res.set_header("Access-Control-Allow-Headers", "Content-Type, Accept");
                if (req.method == "OPTIONS") {
                    res.status = 204;
                    return httplib::Server::HandlerResponse::Handled;
                }
                return httplib::Server::HandlerResponse::Unhandled;
            }
        );
    }

    // GET /api/health
    impl_->server->Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        nlohmann::json result;
        result["status"] = "ok";
        result["sessions"] = GetSessionManager().GetSessionIds().size();
        res.set_content(result.dump(2), "application/json");
    });

    // GET /api/sessions
    impl_->server->Get("/api/sessions", [](const httplib::Request&, httplib::Response& res) {
        auto ids = GetSessionManager().GetSessionIds();
        nlohmann::json sessions = nlohmann::json::array();
        for (const auto& id : ids) {
            if (id == kHeartbeatSessionId || id == kCronSessionId) {
                continue;
            }
            nlohmann::json entry;
            entry["id"] = id;
            entry["busy"] = GetSessionManager().IsSessionBusy(id);
            entry["metadata"] = GetSessionManager().GetSessionMetadata(id);
            sessions.push_back(entry);
        }
        nlohmann::json result;
        result["sessions"] = sessions;
        result["count"] = sessions.size();
        res.set_content(result.dump(2), "application/json");
    });

    // POST /api/sessions
    impl_->server->Post("/api/sessions", [](const httplib::Request& req, httplib::Response& res) {
        std::string sessionId;
        try {
            auto j = nlohmann::json::parse(req.body);
            sessionId = j.value("id", "");
        } catch (...) {
            nlohmann::json err;
            err["error"] = "Invalid JSON";
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
            return;
        }

        if (sessionId.empty()) {
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count() % 10000;
            sessionId = "session_" + std::to_string(ms);
        }

        GetSessionManager().GetOrCreateSession(sessionId);

        nlohmann::json result;
        result["id"] = sessionId;
        result["created"] = true;
        res.status = 201;
        res.set_content(result.dump(2), "application/json");
    });

    // DELETE /api/sessions/{id}
    impl_->server->Delete(R"(/api/sessions/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string sessionId = req.matches[1];

        if (sessionId == kDefaultSessionId || sessionId == kHeartbeatSessionId || sessionId == kCronSessionId) {
            nlohmann::json err;
            err["error"] = "Cannot delete reserved session";
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
            return;
        }

        GetSessionManager().RemoveSession(sessionId);

        nlohmann::json result;
        result["id"] = sessionId;
        result["deleted"] = true;
        res.set_content(result.dump(2), "application/json");

    });

    // POST /api/sessions/{id}/cancel
    impl_->server->Post(R"(/api/sessions/([^/]+)/cancel)", [](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        std::string sessionId = req.matches[1];
        GetSessionManager().Cancel();
        nlohmann::json result;
        result["message"] = "cancel requested";
        result["session_id"] = sessionId;
        res.set_content(result.dump(2), "application/json");
    });

    // GET /api/sessions/{id}/history
    impl_->server->Get(R"(/api/sessions/([^/]+)/history)", [](const httplib::Request& req, httplib::Response& res) {
        std::string sessionId = req.matches[1];

        try {
            auto ctx = GetSessionManager().GetOrCreateSession(sessionId);
            if (!ctx) {
                nlohmann::json err;
                err["error"] = "Session not found";
                res.status = 404;
                res.set_content(err.dump(2), "application/json");
                return;
            }

            auto messages = ctx->GetAllMessages();
            nlohmann::json history = nlohmann::json::array();
            for (const auto& msg : messages) {
                nlohmann::json entry;
                entry["role"] = msg.role;
                entry["content"] = msg.content;
                history.push_back(entry);
            }

            nlohmann::json result;
            result["session_id"] = sessionId;
            result["messages"] = history;
            result["count"] = history.size();
            res.set_content(result.dump(2), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = "Failed to retrieve history";
            err["detail"] = e.what();
            res.status = 500;
            res.set_content(err.dump(2), "application/json");
        }
    });

    // POST /api/chat (non-streaming)
    impl_->server->Post("/api/chat", [](const httplib::Request& req, httplib::Response& res) {
        std::string sessionId;
        std::string message;
        try {
            auto j = nlohmann::json::parse(req.body);
            sessionId = j.value("session_id", kDefaultSessionId);
            message = j.value("message", "");
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = "Invalid JSON";
            err["detail"] = e.what();
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
            return;
        }

        if (message.empty()) {
            nlohmann::json err;
            err["error"] = "message is required";
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
            return;
        }

        SessionInvokeResult result = GetSessionManager().Invoke(
            sessionId,
            message,
            nullptr
        );

        nlohmann::json response;
        response["session_id"] = result.sessionId;
        response["success"] = result.success;
        response["content"] = result.content;
        if (!result.success) {
            response["error"] = result.errorMessage;
        }
        res.set_content(response.dump(2), "application/json");
    });

    // POST /api/chat/stream (SSE streaming)
    impl_->server->Post("/api/chat/stream", [](const httplib::Request& req, httplib::Response& res) {
        std::string sessionId;
        std::string message;
        try {
            auto j = nlohmann::json::parse(req.body);
            sessionId = j.value("session_id", kDefaultSessionId);
            message = j.value("message", "");
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = "Invalid JSON";
            err["detail"] = e.what();
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
            return;
        }

        if (message.empty()) {
            nlohmann::json err;
            err["error"] = "message is required";
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
            return;
        }

        LOG(INFO) << "[WebApi] [" << sessionId << "] Streaming chat requested";

        struct SseContext {
            std::vector<std::string> events;
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
            SessionInvokeResult result;
        };

        auto sseCtx = std::make_shared<SseContext>();

        auto streamCallback = [sseCtx](const std::string& resp) {
            if (resp.empty()) return;

            std::string eventType = "message";
            std::string data = resp;

            if (resp.find("[STREAM]") != std::string::npos) {
                eventType = "stream";
                size_t pos = resp.find("[STREAM]");
                data = resp.substr(pos + 8);
                while (!data.empty() && data[0] == ' ') data = data.substr(1);
            } else if (resp.find("[STATUS]") != std::string::npos) {
                eventType = "status";
                size_t pos = resp.find("[STATUS]");
                data = resp.substr(pos + 8);
                while (!data.empty() && data[0] == ' ') data = data.substr(1);
            } else if (resp.find("[TOOL_CALLS]") != std::string::npos) {
                eventType = "tool_call";
                size_t pos = resp.find("[TOOL_CALLS]");
                data = resp.substr(pos + 12);
                while (!data.empty() && (data[0] == ' ' || data[0] == '\n')) data = data.substr(1);
            } else if (resp.find("[TOOL_RESPONSE]") != std::string::npos) {
                eventType = "tool_response";
                size_t pos = resp.find("[TOOL_RESPONSE]");
                data = resp.substr(pos + 15);
                while (!data.empty() && data[0] == ' ') data = data.substr(1);
            } else if (resp.find("[FINAL]") != std::string::npos) {
                eventType = "done";
                data = "";
            }

            if (!data.empty()) {
                // Format as proper SSE with multiline data support
                std::string sseEvent = "event: " + eventType + "\n";
                size_t start = 0;
                size_t end = data.find('\n');
                while (end != std::string::npos) {
                    sseEvent += "data: " + data.substr(start, end - start) + "\n";
                    start = end + 1;
                    end = data.find('\n', start);
                }
                sseEvent += "data: " + data.substr(start) + "\n\n";

                std::lock_guard<std::mutex> lock(sseCtx->mutex);
                sseCtx->events.push_back(sseEvent);
                sseCtx->cv.notify_one();
            }
        };

        // Run the agent invoke in a background thread
        std::thread([sseCtx, sessionId, message, streamCallback]() {
            try {
                sseCtx->result = GetSessionManager().Invoke(
                    sessionId,
                    message,
                    streamCallback
                );
            } catch (...) {
                LOG(ERR) << "[WebApi] [" << sessionId << "] Agent invoke failed";
            }

            {
                std::lock_guard<std::mutex> lock(sseCtx->mutex);
                sseCtx->done = true;
            }
            sseCtx->cv.notify_one();
        }).detach();

        // Set up chunked response using SSE
        res.set_chunked_content_provider(
            "text/event-stream",
            [sseCtx](size_t /* offset */, httplib::DataSink& sink) -> bool {
                std::unique_lock<std::mutex> lock(sseCtx->mutex);

                sseCtx->cv.wait(lock, [&]() {
                    return !sseCtx->events.empty() || sseCtx->done;
                });

                while (!sseCtx->events.empty()) {
                    const auto& event = sseCtx->events.front();
                    if (!sink.write(event.data(), event.size())) {
                        return false;
                    }
                    sseCtx->events.erase(sseCtx->events.begin());
                }

                if (sseCtx->done) {
                    lock.unlock();

                    nlohmann::json finalResult;
                    finalResult["session_id"] = sseCtx->result.sessionId;
                    finalResult["success"] = sseCtx->result.success;
                    finalResult["content"] = sseCtx->result.content;
                    if (!sseCtx->result.success) {
                        finalResult["error"] = sseCtx->result.errorMessage;
                    }

                    std::string doneEvent = "event: done\ndata: " + finalResult.dump() + "\n\n";
                    if (!sink.write(doneEvent.data(), doneEvent.size())) {
                        return false;
                    }
                    return false;
                }

                return true;
            }
        );
    });

    // ============================================================
    // Channel Management APIs
    // ============================================================

    // GET /api/channels
    impl_->server->Get("/api/channels", [](const httplib::Request&, httplib::Response& res) {
        auto channels = ChannelManager::GetInstance().GetAllChannels();
        nlohmann::json result = nlohmann::json::array();
        for (const auto& ch : channels) {
            nlohmann::json entry;
            entry["id"] = ch.id;
            entry["type"] = ch.type;
            entry["name"] = ch.name;
            entry["enabled"] = ch.enabled;
            entry["params"] = ch.params;
            result.push_back(entry);
        }
        res.set_content(result.dump(2), "application/json");
    });

    // POST /api/channels - create channel
    impl_->server->Post("/api/channels", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = nlohmann::json::parse(req.body);
            ChannelConfig ch;
            ch.id = j.value("id", "");
            ch.type = j.value("type", "");
            ch.name = j.value("name", "");
            ch.enabled = j.value("enabled", true);

            if (j.contains("params")) {
                for (auto& [key, val] : j["params"].items()) {
                    ch.params[key] = val.get<std::string>();
                }
            }

            ChannelManager::GetInstance().AddChannel(ch);
            LOG(INFO) << "[WebApi] Channel created: " << ch.id << " (" << ch.type << ")";

            if (ch.type == "feishu" && ch.enabled) {
                impl_->StartFeishu(ch);
            }

            nlohmann::json result;
            result["id"] = ch.id;
            result["created"] = true;
            res.status = 201;
            res.set_content(result.dump(2), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = "Invalid JSON";
            err["detail"] = e.what();
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
        }
    });

    // DELETE /api/channels/{id}
    impl_->server->Delete(R"(/api/channels/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string channelId = req.matches[1];
        ChannelManager::GetInstance().RemoveChannel(channelId);
        LOG(INFO) << "[WebApi] Channel deleted: " << channelId;

        nlohmann::json result;
        result["id"] = channelId;
        result["deleted"] = true;
        res.set_content(result.dump(2), "application/json");
    });

    // PUT /api/channels/{id}
    impl_->server->Put(R"(/api/channels/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string channelId = req.matches[1];
        try {
            auto j = nlohmann::json::parse(req.body);
            ChannelConfig ch;
            ch.id = channelId;
            ch.type = j.value("type", "");
            ch.name = j.value("name", "");
            ch.enabled = j.value("enabled", true);

            if (j.contains("params")) {
                for (auto& [key, val] : j["params"].items()) {
                    ch.params[key] = val.get<std::string>();
                }
            }

            ChannelManager::GetInstance().UpdateChannel(channelId, ch);
            LOG(INFO) << "[WebApi] Channel updated: " << channelId;

            if (ch.type == "feishu" && ch.enabled) {
                impl_->StartFeishu(ch);
            }

            nlohmann::json result;
            result["id"] = channelId;
            result["updated"] = true;
            res.set_content(result.dump(2), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = "Invalid JSON";
            err["detail"] = e.what();
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
        }
    });

    // Serve static frontend files if configured
    if (!config.staticDir.empty()) {
        impl_->server->set_mount_point("/", config.staticDir);
        LOG(INFO) << "[WebApi] Serving static files from " << config.staticDir;
    }

    // Start server in background thread
    impl_->serverThread = std::thread([this, config]() {
        LOG(INFO) << "[WebApi] Starting server at " << config.host << ":" << config.port;
        if (!impl_->server->listen(config.host.c_str(), config.port)) {
            LOG(ERR) << "[WebApi] Failed to start server";
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    running_ = true;
    LOG(INFO) << "[WebApi] Server running at " << url_;
}

void WebApi::Stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;

    running_ = false;

    if (impl_ && impl_->feishuChannel) {
        impl_->feishuChannel->Stop();
        LOG(INFO) << "[WebApi] Feishu channel stopped";
    }

    if (impl_ && impl_->server) {
        impl_->server->stop();
    }

    if (impl_ && impl_->serverThread.joinable()) {
        impl_->serverThread.join();
    }

    impl_.reset();
    LOG(INFO) << "[WebApi] Stopped";
}

bool WebApi::IsRunning() const
{
    return running_;
}

std::string WebApi::GetUrl() const
{
    return url_;
}

} // namespace jiuwen

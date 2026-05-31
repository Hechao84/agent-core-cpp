#include "examples/jiuwenClaw/adapters/feishu/feishu_bot.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "examples/jiuwenClaw/utils/logger.h"
#include "include/session_manager.h"

#include "third_party/include/nlohmann/json.hpp"

namespace jiuwenClaw {

using namespace jiuwen;

namespace {

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
    LOG(INFO) << "[FeishuBot] Bubble to chat=" << chatId
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
            LOG(INFO) << "[FeishuBot] Agent done, success=" << ctx->result.success;
        } catch (const std::exception& e) {
            LOG(ERR) << "[FeishuBot] InvokeChannel: " << e.what();
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
        LOG(INFO) << "[FeishuBot] Duplicate event skipped: messageId=" << messageId;
        return;
    }

    LOG(INFO) << "[FeishuBot] Event from chat=" << chatId
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

    LOG(INFO) << "[FeishuBot] Invoking agent...";
    SpawnAgentWorker(ctx, msg, streamCb);
    SpawnClosingSender(ctx, channel, chatId);
}

} // namespace

struct FeishuBot::Impl
{
    std::unique_ptr<FeishuChannel> feishuChannel;
    FeishuBotConfig config;
};

FeishuBot::FeishuBot() = default;

FeishuBot::~FeishuBot()
{
    Stop();
}

void FeishuBot::Start(const FeishuBotConfig& config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        LOG(WARN) << "[FeishuBot] Already running";
        return;
    }

    if (config.appId.empty() || config.appSecret.empty()) {
        LOG(WARN) << "[FeishuBot] Skipping Feishu: appId or appSecret missing";
        return;
    }

    impl_ = std::make_unique<Impl>();
    impl_->config = config;

    FeishuConfig fc;
    fc.appId = config.appId;
    fc.appSecret = config.appSecret;

    impl_->feishuChannel = std::make_unique<FeishuChannel>();
    FeishuChannel* channelPtr = impl_->feishuChannel.get();

    impl_->feishuChannel->SetEventCallback(
        [channelPtr](const std::string& chatId, const std::string& messageId,
                     const std::string& senderId, const std::string& msgContent) {
            HandleFeishuEvent(channelPtr, chatId, messageId, senderId, msgContent);
        });

    LOG(INFO) << "[FeishuBot] Starting Feishu long connection: appId=" << fc.appId;
    impl_->feishuChannel->Start(fc);
    if (impl_->feishuChannel->IsRunning()) {
        LOG(INFO) << "[FeishuBot] Feishu channel connected successfully";
    } else {
        LOG(ERR) << "[FeishuBot] Feishu channel FAILED to connect";
        impl_->feishuChannel.reset();
    }

    running_ = true;
}

void FeishuBot::Stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_)
        return;

    running_ = false;

    if (impl_ && impl_->feishuChannel) {
        impl_->feishuChannel->Stop();
        LOG(INFO) << "[FeishuBot] Feishu channel stopped";
    }

    impl_.reset();
    LOG(INFO) << "[FeishuBot] Stopped";
}

bool FeishuBot::IsRunning() const
{
    return running_;
}

} // namespace jiuwenClaw

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "third_party/include/nlohmann/json.hpp"

namespace jiuwenClaw {

// Feishu long connection (WebSocket persistent) configuration.
// Only appId + appSecret are needed.
// No port, no webhook URL, no verification token required.
struct FeishuConfig
{
    std::string appId;
    std::string appSecret;
};

class FeishuChannel
{
public:
    using EventCallback = std::function<void(
        const std::string& chatId,
        const std::string& messageId,
        const std::string& senderId,
        const std::string& messageContent)>;

    FeishuChannel();
    ~FeishuChannel();

    void Start(const FeishuConfig& config);
    void Stop();

    bool IsRunning() const;

    void SetEventCallback(EventCallback callback);

    bool SendCardMessage(const std::string& chatId, const nlohmann::json& card);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::thread connThread_;
    EventCallback eventCallback_;
};

} // namespace jiuwenClaw

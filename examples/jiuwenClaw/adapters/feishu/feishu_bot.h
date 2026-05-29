#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>

#include "examples/jiuwenClaw/adapters/feishu/feishu_channel.h"

namespace jiuwenClaw {

struct FeishuBotConfig
{
    std::string appId;
    std::string appSecret;
};

class FeishuBot
{
public:
    FeishuBot();
    ~FeishuBot();

    void Start(const FeishuBotConfig& config);
    void Stop();

    bool IsRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
};

} // namespace jiuwenClaw

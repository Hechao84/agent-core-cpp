#include "examples/jiuwenClaw/channels/channel_service.h"

#include <utility>

#include "examples/jiuwenClaw/adapters/feishu/feishu_bot.h"
#include "examples/jiuwenClaw/utils/logger.h"

namespace jiuwenClaw {

ChannelService& ChannelService::Instance()
{
    static ChannelService instance;
    return instance;
}

void ChannelService::StartAll()
{
    auto channels = ChannelManager::GetInstance().GetAllChannels();
    for (const auto& ch : channels) {
        Apply(ch);
    }
}

void ChannelService::StopAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : feishuBots_) {
        if (kv.second) {
            kv.second->Stop();
        }
    }
    feishuBots_.clear();
    lastParams_.clear();
    lastTypes_.clear();
    lastEnabled_.clear();
}

void ChannelService::Apply(const ChannelConfig& cfg)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto itType = lastTypes_.find(cfg.id);
    bool typeChanged = (itType != lastTypes_.end()) && (itType->second != cfg.type);
    auto itParam = lastParams_.find(cfg.id);
    bool paramsChanged = (itParam != lastParams_.end()) && (itParam->second != cfg.params);
    auto itEn = lastEnabled_.find(cfg.id);
    bool wasEnabled = itEn != lastEnabled_.end() && itEn->second;
    bool wasRunning = feishuBots_.count(cfg.id) > 0;

    // If already running and any relevant field changed or now disabled,
    // stop first.
    if (wasRunning && (!cfg.enabled || typeChanged || paramsChanged)) {
        StopLocked(cfg.id);
    }

    // Track latest known state regardless of whether we (re)start.
    lastTypes_[cfg.id]   = cfg.type;
    lastParams_[cfg.id]  = cfg.params;
    lastEnabled_[cfg.id] = cfg.enabled;

    if (!cfg.enabled) {
        if (wasEnabled) {
            LOG(INFO) << "[ChannelService] Channel '" << cfg.id << "' disabled";
        }
        return;
    }

    // Start if not running or restarted above.
    if (feishuBots_.count(cfg.id) == 0) {
        if (cfg.type == "feishu") {
            StartFeishuLocked(cfg);
        } else {
            LOG(WARN) << "[ChannelService] Unknown channel type '" << cfg.type
                      << "' for channel id=" << cfg.id;
        }
    }
}

void ChannelService::Remove(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    StopLocked(id);
    lastParams_.erase(id);
    lastTypes_.erase(id);
    lastEnabled_.erase(id);
}

void ChannelService::ReconcileAll()
{
    auto channels = ChannelManager::GetInstance().GetAllChannels();

    // 1. Detect which existing bots are no longer present and stop them.
    std::vector<std::string> currentIds;
    currentIds.reserve(channels.size());
    for (const auto& ch : channels) currentIds.push_back(ch.id);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> toStop;
        for (const auto& kv : feishuBots_) {
            bool stillPresent = false;
            for (const auto& id : currentIds) {
                if (id == kv.first) { stillPresent = true; break; }
            }
            if (!stillPresent) toStop.push_back(kv.first);
        }
        for (const auto& id : toStop) {
            StopLocked(id);
            lastParams_.erase(id);
            lastTypes_.erase(id);
            lastEnabled_.erase(id);
        }
    }

    // 2. Apply/refresh each present channel.
    for (const auto& ch : channels) {
        Apply(ch);
    }
}

std::vector<std::string> ChannelService::ActiveIds() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(feishuBots_.size());
    for (const auto& kv : feishuBots_) {
        if (kv.second && kv.second->IsRunning()) {
            ids.push_back(kv.first);
        }
    }
    return ids;
}

void ChannelService::StartFeishuLocked(const ChannelConfig& cfg)
{
    auto itId  = cfg.params.find("appId");
    auto itSec = cfg.params.find("appSecret");
    std::string appId  = itId  != cfg.params.end() ? itId->second  : "";
    std::string appSec = itSec != cfg.params.end() ? itSec->second : "";
    if (appId.empty() || appSec.empty()) {
        LOG(WARN) << "[ChannelService] Skip Feishu channel '" << cfg.id
                  << "': appId/appSecret missing";
        return;
    }

    FeishuBotConfig bc;
    bc.appId = appId;
    bc.appSecret = appSec;

    auto bot = std::make_unique<FeishuBot>();
    bot->Start(bc);
    if (bot->IsRunning()) {
        LOG(INFO) << "[ChannelService] Feishu channel '" << cfg.id
                  << "' (" << cfg.name << ") connected";
        feishuBots_[cfg.id] = std::move(bot);
    } else {
        LOG(ERR) << "[ChannelService] Feishu channel '" << cfg.id
                 << "' failed to connect";
    }
}

void ChannelService::StopLocked(const std::string& id)
{
    auto it = feishuBots_.find(id);
    if (it == feishuBots_.end()) return;
    if (it->second) {
        it->second->Stop();
    }
    feishuBots_.erase(it);
    LOG(INFO) << "[ChannelService] Channel '" << id << "' stopped";
}

} // namespace jiuwenClaw

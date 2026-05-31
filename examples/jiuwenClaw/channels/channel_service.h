#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "examples/jiuwenClaw/channels/channel_manager.h"

namespace jiuwenClaw {

class FeishuBot;

// Runtime owner of "live" channel adapters (today: FeishuBot).
// Bridges CRUD on ChannelManager to start/stop of the actual adapters.
class ChannelService
{
public:
    static ChannelService& Instance();

    // Start all enabled channels from ChannelManager.
    void StartAll();

    // Stop and clear all bots (call on shutdown).
    void StopAll();

    // (Re)apply a single channel: start if enabled, stop if disabled,
    // restart if running and params changed.
    void Apply(const ChannelConfig& cfg);

    // Stop and remove a channel by id.
    void Remove(const std::string& id);

    // Reconcile all live bots with ChannelManager's current state.
    // Stops bots whose id is no longer present or now disabled, and
    // starts/updates the rest. Useful after a bulk reload.
    void ReconcileAll();

    // Diagnostic.
    std::vector<std::string> ActiveIds() const;

private:
    ChannelService() = default;
    void StartFeishuLocked(const ChannelConfig& cfg);
    void StopLocked(const std::string& id);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<FeishuBot>> feishuBots_;
    // Track effective params per id so we can detect changes.
    std::unordered_map<std::string, std::map<std::string, std::string>> lastParams_;
    std::unordered_map<std::string, std::string> lastTypes_;
    std::unordered_map<std::string, bool> lastEnabled_;
};

} // namespace jiuwenClaw

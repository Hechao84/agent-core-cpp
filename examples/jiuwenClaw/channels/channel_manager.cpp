#include "examples/jiuwenClaw/channels/channel_manager.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "examples/jiuwenClaw/utils/logger.h"

#include "third_party/include/nlohmann/json.hpp"

namespace fs = std::filesystem;

namespace jiuwenClaw {

ChannelManager& ChannelManager::GetInstance()
{
    static ChannelManager instance;
    return instance;
}

void ChannelManager::SetPersistPath(const std::string& path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    persistPath_ = path;
}

void ChannelManager::SaveToFile()
{
    if (persistPath_.empty())
        return;

    try {
        fs::path p(persistPath_);
        if (p.has_parent_path())
            fs::create_directories(p.parent_path());

        nlohmann::json j = nlohmann::json::array();
        for (const auto& pair : channels_) {
            nlohmann::json entry;
            entry["id"] = pair.second.id;
            entry["type"] = pair.second.type;
            entry["name"] = pair.second.name;
            entry["enabled"] = pair.second.enabled;

            nlohmann::json params;
            for (const auto& kv : pair.second.params)
                params[kv.first] = kv.second;
            entry["params"] = params;
            j.push_back(entry);
        }

        std::string tmp = persistPath_ + ".tmp";
        std::ofstream out(tmp, std::ios::trunc);
        out << j.dump(2);
        out.close();
        fs::rename(tmp, persistPath_);
        LOG(INFO) << "[ChannelManager] Saved " << channels_.size()
                  << " channels";
    } catch (const std::exception& e) {
        LOG(ERR) << "[ChannelManager] Save failed: " << e.what();
    }
}

bool ChannelManager::Load()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (persistPath_.empty() || !fs::exists(persistPath_))
        return false;

    try {
        std::ifstream in(persistPath_);
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_discarded() || !j.is_array())
            return false;

        channels_.clear();
        nextId_ = 1;

        for (const auto& item : j) {
            ChannelConfig c;
            c.id = item.value("id", "");
            c.type = item.value("type", "");
            c.name = item.value("name", "");
            c.enabled = item.value("enabled", true);

            if (item.contains("params")) {
                for (auto& kv : item["params"].items())
                    c.params[kv.key()] = kv.value().get<std::string>();
            }
            if (!c.id.empty())
                channels_[c.id] = c;
        }
        RecalcNextId();
        LOG(INFO) << "[ChannelManager] Loaded " << channels_.size()
                  << " channels";
        return true;
    } catch (const std::exception& e) {
        LOG(ERR) << "[ChannelManager] Load failed: " << e.what();
        return false;
    }
}

bool ChannelManager::Save()
{
    std::lock_guard<std::mutex> lock(mutex_);
    SaveToFile();
    return true;
}

void ChannelManager::AddChannel(const ChannelConfig& config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ChannelConfig c = config;
    if (c.id.empty())
        c.id = "channel_" + std::to_string(nextId_++);
    channels_[c.id] = c;
    SaveToFile();
}

void ChannelManager::UpdateChannel(const std::string& id, const ChannelConfig& config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(id);
    if (it != channels_.end()) {
        ChannelConfig c = config;
        c.id = id;
        it->second = c;
        SaveToFile();
    }
}

void ChannelManager::RemoveChannel(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    channels_.erase(id);
    SaveToFile();
}

ChannelConfig* ChannelManager::GetChannel(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(id);
    if (it != channels_.end())
        return &it->second;
    return nullptr;
}

std::vector<ChannelConfig> ChannelManager::GetAllChannels() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ChannelConfig> result;
    for (const auto& p : channels_)
        result.push_back(p.second);
    return result;
}

void ChannelManager::RecalcNextId()
{
    for (const auto& p : channels_) {
        const std::string& id = p.first;
        if (id.rfind("channel_", 0) == 0) {
            try {
                int n = std::stoi(id.substr(8));
                if (n >= nextId_)
                    nextId_ = n + 1;
            } catch (...) {
            }
        }
    }
}

} // namespace jiuwenClaw

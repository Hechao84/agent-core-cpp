#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jiuwenClaw {

struct ChannelConfig
{
    std::string id;
    std::string type;
    std::string name;
    bool enabled{true};
    std::map<std::string, std::string> params;
};

class ChannelManager
{
public:
    static ChannelManager& GetInstance();

    void SetPersistPath(const std::string& path);
    bool Load();
    bool Save();

    void AddChannel(const ChannelConfig& config);
    void UpdateChannel(const std::string& id, const ChannelConfig& config);
    void RemoveChannel(const std::string& id);
    ChannelConfig* GetChannel(const std::string& id);
    std::vector<ChannelConfig> GetAllChannels() const;

private:
    ChannelManager() = default;
    void RecalcNextId();
    void SaveToFile();

    std::string persistPath_;
    mutable std::mutex mutex_;
    std::map<std::string, ChannelConfig> channels_;
    int nextId_{1};
};

} // namespace jiuwenClaw

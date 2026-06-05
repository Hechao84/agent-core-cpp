#include "include/config/agent_config_store.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "include/config/agent_config_json.h"
#include "src/utils/logger.h"

namespace fs = std::filesystem;

namespace jiuwen {

AgentConfigStore& AgentConfigStore::Instance()
{
    static AgentConfigStore instance;
    return instance;
}

void AgentConfigStore::SetPersistPath(const std::string& path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = path;
}

std::string AgentConfigStore::GetPersistPath() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return path_.string();
}

void AgentConfigStore::RegisterDefault(const AgentConfig& def)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (def.id.empty()) {
        LOG(WARN) << "[AgentConfigStore] RegisterDefault: empty id, skipped";
        return;
    }
    defaults_[def.id] = def;
}

nlohmann::json AgentConfigStore::ReadFileLocked()
{
    std::error_code ec;
    if (!fs::exists(path_, ec) || ec) {
        return nlohmann::json{};
    }
    std::ifstream in(path_);
    if (!in.good()) {
        LOG(ERR) << "[AgentConfigStore] Cannot open " << path_.string();
        return nlohmann::json{};
    }
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded()) {
        LOG(ERR) << "[AgentConfigStore] Parse failed: " << path_.string();
        return nlohmann::json{};
    }
    return j;
}

std::unordered_map<std::string, AgentConfig> AgentConfigStore::Load()
{
    std::lock_guard<std::mutex> lock(mutex_);
    overrides_.clear();
    current_.clear();

    nlohmann::json root = ReadFileLocked();
    if (root.is_object() && root.contains("agents") && root["agents"].is_array()) {
        for (const auto& item : root["agents"]) {
            if (!item.is_object() || !item.contains("id")) continue;
            std::string id = item["id"].get<std::string>();
            if (id.empty()) continue;
            overrides_[id] = item;
        }
    }

    // Start from defaults; overlay overrides.
    for (const auto& kv : defaults_) {
        AgentConfig merged = kv.second;
        auto it = overrides_.find(kv.first);
        if (it != overrides_.end()) {
            MergeAgentConfigFromJson(it->second, merged);
        }
        current_[kv.first] = merged;
    }

    // Agents that only appear in JSON (no default) -> still expose them.
    for (const auto& kv : overrides_) {
        if (defaults_.count(kv.first)) continue;
        AgentConfig cfg{};
        cfg.id = kv.first;
        MergeAgentConfigFromJson(kv.second, cfg);
        current_[kv.first] = cfg;
    }

    LOG(INFO) << "[AgentConfigStore] Loaded; defaults=" << defaults_.size()
              << " overrides=" << overrides_.size()
              << " effective=" << current_.size();
    return current_;
}

std::optional<AgentConfig> AgentConfigStore::Get(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = current_.find(id);
    if (it == current_.end()) return std::nullopt;
    return it->second;
}

std::vector<AgentConfig> AgentConfigStore::List() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AgentConfig> out;
    out.reserve(current_.size());
    for (const auto& kv : current_) out.push_back(kv.second);
    return out;
}

void AgentConfigStore::Upsert(const AgentConfig& cfg)
{
    if (cfg.id.empty()) {
        LOG(WARN) << "[AgentConfigStore] Upsert ignored: empty id";
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    overrides_[cfg.id] = AgentConfigToJson(cfg);

    // Recompute effective config for this id.
    AgentConfig merged = cfg;
    auto dIt = defaults_.find(cfg.id);
    if (dIt != defaults_.end()) {
        merged = dIt->second;
        MergeAgentConfigFromJson(overrides_[cfg.id], merged);
    }
    current_[cfg.id] = merged;

    SaveLocked();
}

void AgentConfigStore::UpsertOverride(const std::string& id, const nlohmann::json& overrideJson)
{
    if (id.empty()) {
        LOG(WARN) << "[AgentConfigStore] UpsertOverride ignored: empty id";
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json clean = overrideJson.is_object() ? overrideJson : nlohmann::json::object();
    clean["id"] = id;
    overrides_[id] = clean;

    AgentConfig merged{};
    auto dIt = defaults_.find(id);
    if (dIt != defaults_.end()) {
        merged = dIt->second;
    } else {
        merged.id = id;
    }
    MergeAgentConfigFromJson(overrides_[id], merged);
    current_[id] = merged;
    SaveLocked();
}

void AgentConfigStore::Remove(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    overrides_.erase(id);
    auto dIt = defaults_.find(id);
    if (dIt != defaults_.end()) {
        // Revert to code default.
        current_[id] = dIt->second;
    } else {
        current_.erase(id);
    }
    SaveLocked();
}

bool AgentConfigStore::Save()
{
    std::lock_guard<std::mutex> lock(mutex_);
    SaveLocked();
    return true;
}

void AgentConfigStore::SaveLocked()
{
    try {
        if (path_.has_parent_path()) {
            fs::create_directories(path_.parent_path());
        }
        nlohmann::json root;
        root["version"] = 1;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& kv : overrides_) {
            arr.push_back(kv.second);
        }
        root["agents"] = arr;

        std::string tmp = path_.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << root.dump(2);
        }
        fs::rename(tmp, path_);
        LOG(INFO) << "[AgentConfigStore] Saved overrides count=" << overrides_.size()
                  << " path=" << path_.string();
    } catch (const std::exception& e) {
        LOG(ERR) << "[AgentConfigStore] Save failed: " << e.what();
    }
}

std::filesystem::file_time_type AgentConfigStore::LastWriteTime() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    if (fs::exists(path_, ec) && !ec) {
        return fs::last_write_time(path_, ec);
    }
    return (std::filesystem::file_time_type::min)();
}

bool AgentConfigStore::FileExists() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    return fs::exists(path_, ec) && !ec;
}

} // namespace jiuwen

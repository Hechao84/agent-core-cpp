#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "include/agent_export.h"
#include "include/types.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

// Loads / persists the on-disk override configuration (./data/agents.json)
// and merges it on top of a code-default AgentConfig so the existing
// hard-coded BuildAgentConfig() path keeps working without a config file.
//
// Schema (multi-agent ready):
//   { "version": 1, "agents": [ { "id": "...", ... }, ... ] }
//
// Today only one agent is consumed, but the store stores them all.
class AGENT_API AgentConfigStore
{
public:
    static AgentConfigStore& Instance();

    // Construction is also public to make unit testing of multiple
    // independent stores trivial; production code should use Instance().
    AgentConfigStore() = default;

    void SetPersistPath(const std::string& path);
    std::string GetPersistPath() const;

    // Register a code-default for an agent. Called by main on startup
    // (e.g. RegisterDefault(BuildAgentConfig())).
    void RegisterDefault(const AgentConfig& def);

    // (Re)read the file and produce the effective configs.
    // For each agent id, returns: default + JSON override merged.
    // Newly-discovered ids from JSON without a matching default are
    // returned as-is (parsed from JSON with struct defaults underneath).
    std::unordered_map<std::string, AgentConfig> Load();

    // In-memory accessor for the last Load() result (or default if no Load).
    std::optional<AgentConfig> Get(const std::string& id) const;
    std::vector<AgentConfig> List() const;

    // Write a single agent override back to disk (CRUD via API).
    // Triggers an immediate Save() of the whole agents.json.
    void Upsert(const AgentConfig& cfg);

    // Write a raw JSON override. This is used by Web UI saves so agents.json
    // persists only the fields the UI can edit, instead of serialising the
    // fully-merged effective AgentConfig with all code defaults expanded.
    void UpsertOverride(const std::string& id, const nlohmann::json& overrideJson);
    void Remove(const std::string& id);

    // Persist current in-memory snapshot to disk.
    bool Save();

    // For the watcher.
    std::filesystem::file_time_type LastWriteTime() const;
    bool FileExists() const;

private:
    void SaveLocked();
    nlohmann::json ReadFileLocked();

    mutable std::mutex mutex_;  // Lock layer L5 (config persistence, not on Invoke thread)
    std::filesystem::path path_{"./data/agents.json"};
    std::unordered_map<std::string, AgentConfig> defaults_;
    std::unordered_map<std::string, AgentConfig> current_;
    // Track the JSON overrides explicitly so Save() emits exactly what
    // the user supplied (or what UpsertAgent provided), not the merged
    // result which would lock-in the code defaults forever.
    std::unordered_map<std::string, nlohmann::json> overrides_;
};

} // namespace jiuwen

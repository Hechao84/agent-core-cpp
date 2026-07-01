#include "examples/jiuwenClaw/mcp/mcp_server_manager.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "examples/jiuwenClaw/utils/logger.h"

#include "third_party/include/nlohmann/json.hpp"

namespace fs = std::filesystem;

namespace jiuwenClaw {

McpServerManager& McpServerManager::GetInstance()
{
    static McpServerManager instance;
    return instance;
}

void McpServerManager::SetPersistPath(const std::string& path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    persistPath_ = path;
}

void McpServerManager::SaveToFile()
{
    if (persistPath_.empty()) {
        return;
    }

    try {
        fs::path p(persistPath_);
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path());
        }

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& pair : servers_) {
            const auto& e = pair.second;
            nlohmann::json item;
            item["id"]          = e.id;
            item["name"]        = e.name;
            item["description"] = e.description;
            item["enabled"]     = e.enabled;
            item["type"]        = e.type;
            item["url"]         = e.url;
            item["endpoint"]    = e.endpoint;
            item["command"]     = e.command;
            item["args"]        = e.args;
            item["connectTimeoutSeconds"] = e.connectTimeoutSeconds;
            item["requestTimeoutSeconds"] = e.requestTimeoutSeconds;

            nlohmann::json env = nlohmann::json::object();
            for (const auto& kv : e.env) {
                env[kv.first] = kv.second;
            }
            item["env"] = env;

            nlohmann::json headers = nlohmann::json::object();
            for (const auto& kv : e.headers) {
                headers[kv.first] = kv.second;
            }
            item["headers"] = headers;

            arr.push_back(item);
        }

        nlohmann::json doc;
        doc["version"] = 1;
        doc["servers"] = arr;

        std::string tmp = persistPath_ + ".tmp";
        std::ofstream out(tmp, std::ios::trunc);
        out << doc.dump(2);
        out.close();
        fs::rename(tmp, persistPath_);
        LOG(INFO) << "[McpServerManager] Saved " << servers_.size()
                  << " MCP server(s) to " << persistPath_;
    } catch (const std::exception& ex) {
        LOG(ERR) << "[McpServerManager] Save failed: " << ex.what();
    }
}

bool McpServerManager::Load()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (persistPath_.empty() || !fs::exists(persistPath_)) {
        return false;
    }

    try {
        std::ifstream in(persistPath_);
        nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
        if (doc.is_discarded()) {
            return false;
        }

        // Support both top-level array (legacy) and {version, servers: [...]} form.
        const nlohmann::json* arr = nullptr;
        if (doc.is_array()) {
            arr = &doc;
        } else if (doc.is_object() && doc.contains("servers") && doc["servers"].is_array()) {
            arr = &doc["servers"];
        } else {
            return false;
        }

        servers_.clear();
        for (const auto& item : *arr) {
            if (!item.is_object()) {
                continue;
            }
            McpServerEntry e;
            e.id          = item.value("id", "");
            e.name        = item.value("name", "");
            e.description = item.value("description", "");
            e.enabled     = item.value("enabled", true);
            e.type        = item.value("type", "");
            e.url         = item.value("url", "");
            e.endpoint    = item.value("endpoint", "");
            e.command     = item.value("command", "");
            e.connectTimeoutSeconds = item.value("connectTimeoutSeconds", 3);
            e.requestTimeoutSeconds = item.value("requestTimeoutSeconds", 10);

            if (item.contains("args") && item["args"].is_array()) {
                for (const auto& a : item["args"]) {
                    if (a.is_string()) {
                        e.args.push_back(a.get<std::string>());
                    }
                }
            }
            if (item.contains("env") && item["env"].is_object()) {
                for (auto it = item["env"].begin(); it != item["env"].end(); ++it) {
                    if (it.value().is_string()) {
                        e.env[it.key()] = it.value().get<std::string>();
                    }
                }
            }
            if (item.contains("headers") && item["headers"].is_object()) {
                for (auto it = item["headers"].begin(); it != item["headers"].end(); ++it) {
                    if (it.value().is_string()) {
                        e.headers[it.key()] = it.value().get<std::string>();
                    }
                }
            }
            if (!e.id.empty()) {
                servers_[e.id] = e;
            }
        }

        LOG(INFO) << "[McpServerManager] Loaded " << servers_.size()
                  << " MCP server(s) from " << persistPath_;
        return true;
    } catch (const std::exception& ex) {
        LOG(ERR) << "[McpServerManager] Load failed: " << ex.what();
        return false;
    }
}

bool McpServerManager::Save()
{
    std::lock_guard<std::mutex> lock(mutex_);
    SaveToFile();
    return true;
}

void McpServerManager::AddServer(const McpServerEntry& entry)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry.id.empty()) {
        return;
    }
    servers_[entry.id] = entry;
    SaveToFile();
}

void McpServerManager::UpdateServer(const std::string& id, const McpServerEntry& entry)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = servers_.find(id);
    if (it != servers_.end()) {
        McpServerEntry e = entry;
        e.id = id;
        it->second = e;
        SaveToFile();
    }
}

void McpServerManager::RemoveServer(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    servers_.erase(id);
    SaveToFile();
}

McpServerEntry* McpServerManager::GetServer(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = servers_.find(id);
    if (it != servers_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<McpServerEntry> McpServerManager::GetAllServers() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<McpServerEntry> out;
    out.reserve(servers_.size());
    for (const auto& p : servers_) {
        out.push_back(p.second);
    }
    return out;
}

jiuwen::McpServerConfig McpServerManager::EntryToConfig(const McpServerEntry& e)
{
    jiuwen::McpServerConfig c;
    c.id          = e.id;
    c.name        = e.name;
    c.description = e.description;
    c.enabled     = e.enabled;
    c.type        = e.type;
    c.url         = e.url;
    c.endpoint    = e.endpoint;
    c.command     = e.command;
    c.args        = e.args;
    c.env         = e.env;
    c.headers     = e.headers;
    c.connectTimeoutSeconds = e.connectTimeoutSeconds;
    c.requestTimeoutSeconds = e.requestTimeoutSeconds;
    return c;
}

std::vector<jiuwen::McpServerConfig> McpServerManager::ToFrameworkConfigs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<jiuwen::McpServerConfig> out;
    for (const auto& p : servers_) {
        if (p.second.enabled) {
            out.push_back(EntryToConfig(p.second));
        }
    }
    return out;
}

bool McpServerManager::ToFrameworkConfig(const std::string& id, jiuwen::McpServerConfig& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = servers_.find(id);
    if (it == servers_.end()) {
        return false;
    }
    out = EntryToConfig(it->second);
    return true;
}

} // namespace jiuwenClaw

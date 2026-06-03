#include "include/config/agent_config_json.h"

#include <algorithm>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace jiuwen {

// === enum string mapping ===

std::string WorkModeToString(AgentWorkMode mode)
{
    switch (mode) {
        case AgentWorkMode::REACT:            return "react";
        case AgentWorkMode::PLAN_AND_EXECUTE: return "plan_and_execute";
        case AgentWorkMode::WORKFLOW:         return "workflow";
    }
    return "react";
}

bool WorkModeFromString(const std::string& s, AgentWorkMode& out)
{
    if (s == "react")            { out = AgentWorkMode::REACT; return true; }
    if (s == "plan_and_execute") { out = AgentWorkMode::PLAN_AND_EXECUTE; return true; }
    if (s == "workflow")         { out = AgentWorkMode::WORKFLOW; return true; }
    return false;
}

std::string FormatTypeToString(ModelFormatType t)
{
    switch (t) {
        case ModelFormatType::OPENAI:    return "openai";
        case ModelFormatType::ANTHROPIC: return "anthropic";
    }
    return "openai";
}

bool FormatTypeFromString(const std::string& s, ModelFormatType& out)
{
    if (s == "openai")    { out = ModelFormatType::OPENAI; return true; }
    if (s == "anthropic") { out = ModelFormatType::ANTHROPIC; return true; }
    return false;
}

std::string PromptResourceTypeToString(PromptResourceType t)
{
    switch (t) {
        case PromptResourceType::TEXT:      return "TEXT";
        case PromptResourceType::FILE_PATH: return "FILE_PATH";
    }
    return "TEXT";
}

bool PromptResourceTypeFromString(const std::string& s, PromptResourceType& out)
{
    if (s == "TEXT")      { out = PromptResourceType::TEXT; return true; }
    if (s == "FILE_PATH") { out = PromptResourceType::FILE_PATH; return true; }
    return false;
}

std::string StorageTypeToString(ContextConfig::StorageType t)
{
    switch (t) {
        case ContextConfig::StorageType::MEMORY_ONLY: return "MEMORY_ONLY";
        case ContextConfig::StorageType::JSON_FILE:   return "JSON_FILE";
        case ContextConfig::StorageType::DATABASE:    return "DATABASE";
    }
    return "JSON_FILE";
}

bool StorageTypeFromString(const std::string& s, ContextConfig::StorageType& out)
{
    if (s == "MEMORY_ONLY") { out = ContextConfig::StorageType::MEMORY_ONLY; return true; }
    if (s == "JSON_FILE")   { out = ContextConfig::StorageType::JSON_FILE; return true; }
    if (s == "DATABASE")    { out = ContextConfig::StorageType::DATABASE; return true; }
    return false;
}

// ConfigNode <-> JSON helpers (recursive)
nlohmann::json ConfigNodeToJson(const ConfigNode& node)
{
    nlohmann::json j = nlohmann::json::object();
    for (const auto& kv : node.fields_) {
        const auto& val = kv.second;
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::shared_ptr<ConfigNode>>) {
                j[kv.first] = v ? ConfigNodeToJson(*v) : nlohmann::json::object();
            } else {
                j[kv.first] = v;
            }
        }, val);
    }
    return j;
}

void ConfigNodeFromJson(const nlohmann::json& j, ConfigNode& out)
{
    if (!j.is_object()) {
        return;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& key = it.key();
        const auto& v = it.value();
        if (v.is_boolean()) {
            out.fields_[key] = v.get<bool>();
        } else if (v.is_number_integer()) {
            out.fields_[key] = v.get<int>();
        } else if (v.is_number_float()) {
            out.fields_[key] = v.get<float>();
        } else if (v.is_string()) {
            out.fields_[key] = v.get<std::string>();
        } else if (v.is_array()) {
            std::vector<std::string> arr;
            for (const auto& item : v) {
                if (item.is_string()) arr.push_back(item.get<std::string>());
                else arr.push_back(item.dump());
            }
            out.fields_[key] = arr;
        } else if (v.is_object()) {
            auto child = std::make_shared<ConfigNode>();
            ConfigNodeFromJson(v, *child);
            out.fields_[key] = child;
        }
    }
}

// === Per-type merge helpers (override-only) ===

static void MergeModelConfig(const nlohmann::json& j, ModelConfig& out)
{
    if (!j.is_object()) return;
    if (j.contains("baseUrl"))   out.baseUrl   = j["baseUrl"].get<std::string>();
    if (j.contains("apiKey"))    out.apiKey    = j["apiKey"].get<std::string>();
    if (j.contains("modelName")) out.modelName = j["modelName"].get<std::string>();
    if (j.contains("provider"))  out.provider  = j["provider"].get<std::string>();
    if (j.contains("formatType") && j["formatType"].is_string()) {
        ModelFormatType ft;
        if (FormatTypeFromString(j["formatType"].get<std::string>(), ft)) {
            out.formatType = ft;
        }
    }
    if (j.contains("extraParams") && j["extraParams"].is_object()) {
        out.extraParams = ConfigNode{};
        ConfigNodeFromJson(j["extraParams"], out.extraParams);
    }
}

static nlohmann::json ModelConfigToJson(const ModelConfig& cfg)
{
    nlohmann::json j;
    j["baseUrl"]    = cfg.baseUrl;
    j["apiKey"]     = cfg.apiKey;
    j["modelName"]  = cfg.modelName;
    j["provider"]   = cfg.provider;
    j["formatType"] = FormatTypeToString(cfg.formatType);
    j["extraParams"] = ConfigNodeToJson(cfg.extraParams);
    return j;
}

static void MergeContextConfig(const nlohmann::json& j, ContextConfig& out)
{
    if (!j.is_object()) return;
    if (j.contains("maxContextTokens")) out.maxContextTokens = j["maxContextTokens"].get<int>();
    if (j.contains("maxMessages"))      out.maxMessages      = j["maxMessages"].get<int>();
    if (j.contains("sessionId"))        out.sessionId        = j["sessionId"].get<std::string>();
    if (j.contains("storagePath"))      out.storagePath      = j["storagePath"].get<std::string>();
    if (j.contains("storageType") && j["storageType"].is_string()) {
        ContextConfig::StorageType st;
        if (StorageTypeFromString(j["storageType"].get<std::string>(), st)) {
            out.storageType = st;
        }
    }
    if (j.contains("enableSummarization"))      out.enableSummarization      = j["enableSummarization"].get<bool>();
    if (j.contains("idleConsolidationSeconds")) out.idleConsolidationSeconds = j["idleConsolidationSeconds"].get<int>();
}

static nlohmann::json ContextConfigToJson(const ContextConfig& cfg)
{
    nlohmann::json j;
    j["maxContextTokens"]         = cfg.maxContextTokens;
    j["maxMessages"]              = cfg.maxMessages;
    j["sessionId"]                = cfg.sessionId;
    j["storagePath"]              = cfg.storagePath;
    j["storageType"]              = StorageTypeToString(cfg.storageType);
    j["enableSummarization"]      = cfg.enableSummarization;
    j["idleConsolidationSeconds"] = cfg.idleConsolidationSeconds;
    return j;
}

static void MergeDreamConfig(const nlohmann::json& j, DreamConfig& out)
{
    if (!j.is_object()) return;
    if (j.contains("dataBasePath")) {
        out.dataBasePath = j["dataBasePath"].get<std::string>();
    }
    if (j.contains("historyPath")) {
        out.historyPath = j["historyPath"].get<std::string>();
    }
    if (j.contains("maxBatchSize")) {
        out.maxBatchSize = j["maxBatchSize"].get<int>();
    }
    if (j.contains("maxIterations")) {
        out.maxIterations = j["maxIterations"].get<int>();
    }
    if (j.contains("maxToolResultChars")) {
        out.maxToolResultChars = j["maxToolResultChars"].get<int>();
    }
    if (j.contains("historyEntryPreviewMaxChars")) {
        out.historyEntryPreviewMaxChars = j["historyEntryPreviewMaxChars"].get<int>();
    }
    if (j.contains("memoryFileMaxChars")) {
        out.memoryFileMaxChars = j["memoryFileMaxChars"].get<int>();
    }
}

static nlohmann::json DreamConfigToJson(const DreamConfig& cfg)
{
    nlohmann::json j;
    j["dataBasePath"]                = cfg.dataBasePath;
    j["historyPath"]                 = cfg.historyPath;
    j["maxBatchSize"]                = cfg.maxBatchSize;
    j["maxIterations"]               = cfg.maxIterations;
    j["maxToolResultChars"]          = cfg.maxToolResultChars;
    j["historyEntryPreviewMaxChars"] = cfg.historyEntryPreviewMaxChars;
    j["memoryFileMaxChars"]          = cfg.memoryFileMaxChars;
    return j;
}

static void MergePromptTemplates(const nlohmann::json& j,
    std::unordered_map<std::string, PromptResource>& out)
{
    if (!j.is_object()) return;
    // Override semantics: any key listed in JSON overwrites; unlisted keys preserved.
    for (auto it = j.begin(); it != j.end(); ++it) {
        const auto& v = it.value();
        PromptResource pr;
        if (v.is_string()) {
            pr.type = PromptResourceType::TEXT;
            pr.value = v.get<std::string>();
        } else if (v.is_object()) {
            if (v.contains("type") && v["type"].is_string()) {
                PromptResourceType pt;
                if (PromptResourceTypeFromString(v["type"].get<std::string>(), pt)) {
                    pr.type = pt;
                }
            }
            if (v.contains("value") && v["value"].is_string()) {
                pr.value = v["value"].get<std::string>();
            }
        } else {
            continue;
        }
        out[it.key()] = pr;
    }
}

static nlohmann::json PromptTemplatesToJson(
    const std::unordered_map<std::string, PromptResource>& m)
{
    nlohmann::json j = nlohmann::json::object();
    for (const auto& kv : m) {
        nlohmann::json e;
        e["type"]  = PromptResourceTypeToString(kv.second.type);
        e["value"] = kv.second.value;
        j[kv.first] = e;
    }
    return j;
}

// === Top-level AgentConfig ===

void MergeAgentConfigFromJson(const nlohmann::json& j, AgentConfig& out)
{
    if (!j.is_object()) return;

    if (j.contains("id"))          out.id          = j["id"].get<std::string>();
    if (j.contains("name"))        out.name        = j["name"].get<std::string>();
    if (j.contains("description")) out.description = j["description"].get<std::string>();
    if (j.contains("version"))     out.version     = j["version"].get<std::string>();
    if (j.contains("mode") && j["mode"].is_string()) {
        AgentWorkMode m;
        if (WorkModeFromString(j["mode"].get<std::string>(), m)) {
            out.mode = m;
        }
    }
    if (j.contains("modelConfig"))    MergeModelConfig(j["modelConfig"], out.modelConfig);
    if (j.contains("contextConfig"))  MergeContextConfig(j["contextConfig"], out.contextConfig);
    if (j.contains("dreamConfig"))    MergeDreamConfig(j["dreamConfig"], out.dreamConfig);
    if (j.contains("promptTemplates")) MergePromptTemplates(j["promptTemplates"], out.promptTemplates);
    if (j.contains("skillDirectory"))         out.skillDirectory        = j["skillDirectory"].get<std::string>();
    if (j.contains("maxIterations"))          out.maxIterations         = j["maxIterations"].get<int>();
    if (j.contains("dataBasePath"))           out.dataBasePath          = j["dataBasePath"].get<std::string>();
    if (j.contains("maxConcurrentSessions"))  out.maxConcurrentSessions = j["maxConcurrentSessions"].get<int>();
    if (j.contains("defaultTools") && j["defaultTools"].is_array()) {
        std::vector<std::string> tools;
        for (const auto& t : j["defaultTools"]) {
            if (t.is_string()) tools.push_back(t.get<std::string>());
        }
        out.defaultTools = std::move(tools);
    }
    if (j.contains("mcpServerIds") && j["mcpServerIds"].is_array()) {
        std::vector<std::string> ids;
        for (const auto& v : j["mcpServerIds"]) {
            if (v.is_string()) ids.push_back(v.get<std::string>());
        }
        out.mcpServerIds = std::move(ids);
    }
}

nlohmann::json AgentConfigToJson(const AgentConfig& cfg)
{
    nlohmann::json j;
    j["id"]                    = cfg.id;
    j["name"]                  = cfg.name;
    j["description"]           = cfg.description;
    j["version"]               = cfg.version;
    j["mode"]                  = WorkModeToString(cfg.mode);
    j["modelConfig"]           = ModelConfigToJson(cfg.modelConfig);
    j["contextConfig"]         = ContextConfigToJson(cfg.contextConfig);
    j["dreamConfig"]           = DreamConfigToJson(cfg.dreamConfig);
    j["promptTemplates"]       = PromptTemplatesToJson(cfg.promptTemplates);
    j["skillDirectory"]        = cfg.skillDirectory;
    j["maxIterations"]         = cfg.maxIterations;
    j["dataBasePath"]          = cfg.dataBasePath;
    j["maxConcurrentSessions"] = cfg.maxConcurrentSessions;
    j["defaultTools"]          = cfg.defaultTools;
    j["mcpServerIds"]          = cfg.mcpServerIds;
    return j;
}

AgentConfig MergeAgentConfig(const AgentConfig& base, const nlohmann::json& overrideJson)
{
    AgentConfig merged = base;
    MergeAgentConfigFromJson(overrideJson, merged);
    return merged;
}

} // namespace jiuwen

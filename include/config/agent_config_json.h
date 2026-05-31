#pragma once

#include <string>

#include "include/agent_export.h"
#include "include/types.h"

#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

// Serialize / deserialize AgentConfig and friends to nlohmann::json.
// Unknown fields are ignored on parse; missing fields fall back to defaults
// in the destination object (i.e. the merge target).
AGENT_API nlohmann::json AgentConfigToJson(const AgentConfig& cfg);

// Parse a JSON object into 'out'. Fields not present in JSON are left
// unchanged on 'out'. This is the cornerstone of "default + override merge":
// pass the code default as 'out', then call MergeFromJson(json, out).
AGENT_API void MergeAgentConfigFromJson(const nlohmann::json& j, AgentConfig& out);

// Convenience: build an AgentConfig by merging 'overrideJson' on top of 'base'.
AGENT_API AgentConfig MergeAgentConfig(const AgentConfig& base, const nlohmann::json& overrideJson);

// Helpers (also exposed for tests).
AGENT_API std::string WorkModeToString(AgentWorkMode mode);
AGENT_API bool WorkModeFromString(const std::string& s, AgentWorkMode& out);

AGENT_API std::string FormatTypeToString(ModelFormatType t);
AGENT_API bool FormatTypeFromString(const std::string& s, ModelFormatType& out);

AGENT_API std::string PromptResourceTypeToString(PromptResourceType t);
AGENT_API bool PromptResourceTypeFromString(const std::string& s, PromptResourceType& out);

AGENT_API std::string StorageTypeToString(ContextConfig::StorageType t);
AGENT_API bool StorageTypeFromString(const std::string& s, ContextConfig::StorageType& out);

// ConfigNode <-> JSON (used by ModelConfig.extraParams).
AGENT_API nlohmann::json ConfigNodeToJson(const ConfigNode& node);
AGENT_API void ConfigNodeFromJson(const nlohmann::json& j, ConfigNode& out);

} // namespace jiuwen

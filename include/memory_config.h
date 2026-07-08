#pragma once

#include <string>
#include <vector>

#include "include/config_node.h"

namespace jiuwen {

// Runtime configuration for the memory subsystem. Shared by the core framework
// and memory plugins, so it is intentionally kept in a dedicated header free of
// unrelated framework configuration types.
struct MemoryConfig
{
    bool enabled{true};
    std::string mode{"sdk"};
    std::string provider{"builtin.compat"};
    std::string dataPath;

    std::string serverUrl;
    std::string serverApiKey;
    int serverTimeoutSeconds{10};
    int serverMaxRetries{2};                 // HTTP 瞬态错误（5xx/timeout）重试次数
    int serverCircuitThreshold{5};           // 连续失败达到此数打开熔断
    int serverCircuitCooldownSeconds{30};    // 熔断打开后冷却时间

    int tokenBudget{4096};
    int offloadToolResultChars{8000};
    // When true, tool results at or above offloadToolResultChars are written to
    // a payload file and replaced in context by a short summary plus a payload
    // reference, which the model can re-read on demand via the
    // memory_read_payload tool. This saves context tokens for large outputs.
    // Requires the memory_read_payload tool to be available in the session so
    // the offloaded content remains reachable.
    bool enablePayloadOffload{true};

    // Idle seconds between memory consolidation passes. Drives the
    // Agent::ConsolidationLoop poll interval. Semantically a memory-policy
    // knob (not context-engine state), so it lives here rather than in
    // ContextConfig. JSON deserialization reads from `memoryConfig.idleConsolidationSeconds`
    // first and falls back to `contextConfig.idleConsolidationSeconds` for
    // backward compatibility with older config files.
    int idleConsolidationSeconds{60};

    // Session ids that must be skipped by memory consolidation. Events
    // belonging to these sessions are still persisted (audit trail) and
    // still advance the consolidation cursor, but they never enter the
    // consolidation batch and do not trigger hasNewActivity_. Empty by
    // default; application layers populate this with their own system
    // session ids (e.g. cron / heartbeat reserved sessions) so the long-term
    // memory reflects real user conversations rather than mechanical ticks.
    std::vector<std::string> excludedConsolidationSessionIds;

    // Runtime-owned (built-in) model for memory consolidation.
    // - false (default): the runtime does NOT load its own model. LLM-backed
    //   consolidation still happens because the host (Agent::ConsolidationLoop)
    //   injects its own model via MemoryModelClient on each Consolidate call.
    // - true: the runtime additionally loads the model described by the
    //   model* fields below as its own internal client. Only set this when you
    //   want the runtime to own a separate model independent of the host.
    bool modelEnabled{false};
    std::string modelFormatType{"openai"};
    std::string modelBaseUrl;
    std::string modelApiKey;
    std::string modelName;
    std::string modelOrganization;
    std::string modelAnthropicVersion{"2023-06-01"};
    int modelTimeoutSeconds{60};
    double modelTemperature{0.0};
    int modelMaxTokens{0};

    ConfigNode extraParams;
};

} // namespace jiuwen

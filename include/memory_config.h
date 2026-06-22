#pragma once

#include <string>

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

    int tokenBudget{4096};
    int offloadToolResultChars{8000};
    // When true, tool results at or above offloadToolResultChars are written to
    // a payload file and replaced in context by a short summary plus a payload
    // reference, which the model can re-read on demand via the
    // memory_read_payload tool. This saves context tokens for large outputs.
    // Requires the memory_read_payload tool to be available in the session so
    // the offloaded content remains reachable.
    bool enablePayloadOffload{true};

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

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
    bool enablePayloadOffload{false};

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

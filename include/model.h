#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "include/agent_export.h"
#include "include/types.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

// A single tool invocation produced by the model. id is the canonical
// identifier that ties the assistant tool-call to the tool result message.
// In native function-calling mode the id is returned by the server; in
// fallback (prompt-only) mode the worker generates a local id.
struct ToolCall {
    std::string id;
    std::string name;
    std::string argumentsJson;  // JSON-encoded string (OpenAI spec compatible)
};

// A function schema published to the model so it knows what is callable.
// parameters is a JSON Schema object describing the arguments.
struct ToolSchema {
    std::string name;
    std::string description;
    nlohmann::json parameters;
};

// Conversation message. Persisted and exchanged across the framework.
// Roles:
//   system    : top-level system instructions
//   user      : end-user input
//   assistant : model output - either content text, tool_calls, or both
//   tool      : observation produced by executing a single tool call
struct Message {
    std::string role;
    std::string content;
    std::vector<ToolCall> toolCalls;  // assistant only
    std::string toolCallId;           // tool only
    std::string toolName;             // tool only (informational)
};

struct ModelResponse {
    std::string content;
    std::vector<ToolCall> toolCalls;
    bool isFinished{false};
    std::string finishReason;  // "stop" | "tool_calls" | "length" | provider-specific
};

class AGENT_API Model {
public:
    explicit Model(ModelConfig config);
    virtual ~Model() = default;

    // Format the request body sent to the provider. tools may be empty.
    virtual std::string Format(const std::string& systemPrompt,
                                const std::vector<Message>& messages,
                                const std::vector<ToolSchema>& tools) = 0;

    // Invoke the provider with the pre-built body. onChunk receives streamed
    // text deltas (content tokens only); tool_calls / tool_use are buffered
    // and returned through ModelResponse.toolCalls.
    virtual ModelResponse Invoke(const std::string& formattedInput,
                                  std::function<void(const std::string&)> onChunk) = 0;

    ModelConfig GetConfig() const;

protected:
    ModelConfig config_;
};

} // namespace jiuwen

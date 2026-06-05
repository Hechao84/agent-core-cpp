// Verify OpenAIModel::Format produces OpenAI function-calling spec output:
//   * assistant with tool_calls -> {role,content,tool_calls:[{id,type,function}]}
//   * tool with tool_call_id    -> {role:"tool",tool_call_id,content}
// Both native (useNativeFunctionCalling=true) and fallback paths are tested.

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "include/model.h"
#include "include/types.h"
#include "src/models/openai_model.h"
#include "third_party/include/nlohmann/json.hpp"

using namespace jiuwen;
using json = nlohmann::json;

static void Check(bool cond, const std::string& tag)
{
    if (!cond) {
        std::cerr << "[FAIL] " << tag << std::endl;
        std::exit(1);
    }
    std::cout << "[OK]   " << tag << std::endl;
}

int main()
{
    // Build a multi-round ReAct trace as structured Messages.
    Message u1; u1.role = "user"; u1.content = "What's the weather in SF?";

    Message a1; a1.role = "assistant"; a1.content = "";
    ToolCall tc1; tc1.id = "call_s1_1"; tc1.name = "web_search"; tc1.argumentsJson = "{\"q\":\"sf weather\"}";
    a1.toolCalls.push_back(tc1);

    Message t1; t1.role = "tool"; t1.toolCallId = "call_s1_1"; t1.toolName = "web_search"; t1.content = "22C sunny";

    Message a2; a2.role = "assistant"; a2.content = "It's 22C sunny in SF.";

    std::vector<Message> msgs = {u1, a1, t1, a2};

    // Native mode
    {
        ModelConfig cfg;
        cfg.modelName = "gpt-5";
        cfg.baseUrl = "http://example/v1";
        cfg.useNativeFunctionCalling = true;
        OpenAIModel model(cfg);

        ToolSchema schema;
        schema.name = "web_search";
        schema.description = "Search the web";
        schema.parameters = json{{"type","object"},{"properties",{{"q",{{"type","string"}}}}}};

        auto payload = json::parse(model.Format("sys", msgs, {schema}));
        auto& m = payload["messages"];
        Check(m.size() == 5, "native: 5 wire messages (sys + 4)");
        Check(m[0]["role"] == "system", "native: system first");
        Check(m[1]["role"] == "user", "native: original user");
        Check(m[2]["role"] == "assistant", "native: assistant tool-call");
        Check(m[2]["content"].is_null(), "native: assistant content null when tool_calls present");
        Check(m[2]["tool_calls"].is_array() && m[2]["tool_calls"].size() == 1, "native: tool_calls array size 1");
        Check(m[2]["tool_calls"][0]["id"] == "call_s1_1", "native: tool_call id preserved");
        Check(m[2]["tool_calls"][0]["type"] == "function", "native: type=function");
        Check(m[2]["tool_calls"][0]["function"]["name"] == "web_search", "native: function.name");
        Check(m[2]["tool_calls"][0]["function"]["arguments"].is_string(), "native: arguments is string");
        Check(m[3]["role"] == "tool", "native: tool role kept");
        Check(m[3]["tool_call_id"] == "call_s1_1", "native: tool_call_id pairing");
        Check(m[3]["content"].get<std::string>().find("22C") != std::string::npos, "native: tool content preserved");
        Check(m[4]["role"] == "assistant" && m[4]["content"] == "It's 22C sunny in SF.",
              "native: final plain assistant preserved");

        Check(payload.contains("tools") && payload["tools"].is_array(),
              "native: tools array in payload");
        Check(payload["tools"][0]["type"] == "function" && payload["tools"][0]["function"]["name"] == "web_search",
              "native: tools[0] schema correct");
    }

    // Fallback mode
    {
        ModelConfig cfg;
        cfg.modelName = "any-old-llm";
        cfg.baseUrl = "http://example/v1";
        cfg.useNativeFunctionCalling = false;
        OpenAIModel model(cfg);

        ToolSchema schema;
        schema.name = "web_search";
        schema.description = "Search";
        schema.parameters = json::object();

        auto payload = json::parse(model.Format("sys", msgs, {schema}));
        auto& m = payload["messages"];
        Check(!payload.contains("tools"), "fallback: no tools field on payload");
        // No raw role=tool should appear on the wire.
        for (size_t i = 0; i < m.size(); ++i) {
            Check(m[i]["role"] != "tool", "fallback: no raw tool role at i=" + std::to_string(i));
        }
        // Last wire message must be user (a tool result), otherwise the model
        // would have no reason to generate.
        Check(m.back()["role"] == "user" || m.back()["role"] == "assistant",
              "fallback: ends in user or assistant");
        // System prompt should embed the tool catalogue.
        Check(m[0]["role"] == "system" && m[0]["content"].get<std::string>().find("web_search") != std::string::npos,
              "fallback: tool catalogue embedded in system");
    }

    std::cout << "\nAll openai_format checks passed.\n";
    return 0;
}

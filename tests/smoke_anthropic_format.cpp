// Verify AnthropicModel::Format produces Anthropic native function-calling
// wire shape: assistant.content = [{text}, {tool_use, id, name, input}],
// tool results folded into a user.content with [{tool_result, tool_use_id, content}].

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "include/model.h"
#include "include/types.h"
#include "src/models/anthropic_model.h"
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
    Message u; u.role = "user"; u.content = "Find weather and time";

    Message a1; a1.role = "assistant"; a1.content = "";
    ToolCall tc1; tc1.id = "toolu_a"; tc1.name = "weather"; tc1.argumentsJson = "{\"city\":\"sf\"}";
    ToolCall tc2; tc2.id = "toolu_b"; tc2.name = "now"; tc2.argumentsJson = "{}";
    a1.toolCalls = {tc1, tc2};

    Message t1; t1.role = "tool"; t1.toolCallId = "toolu_a"; t1.toolName = "weather"; t1.content = "22C sunny";
    Message t2; t2.role = "tool"; t2.toolCallId = "toolu_b"; t2.toolName = "now"; t2.content = "2026-06-04";

    Message a2; a2.role = "assistant"; a2.content = "SF is 22C, today is 2026-06-04.";

    std::vector<Message> msgs = {u, a1, t1, t2, a2};

    // Native mode
    {
        ModelConfig cfg;
        cfg.modelName = "claude-3-7-sonnet";
        cfg.baseUrl = "https://api.anthropic.com";
        cfg.useNativeFunctionCalling = true;
        AnthropicModel model(cfg);

        ToolSchema s1; s1.name = "weather"; s1.description = "Get weather";
        s1.parameters = json{{"type","object"},{"properties",{{"city",{{"type","string"}}}}}};
        ToolSchema s2; s2.name = "now"; s2.description = "Get current time";
        s2.parameters = json{{"type","object"},{"properties", json::object()}};

        auto payload = json::parse(model.Format("sys", msgs, {s1, s2}));
        auto& m = payload["messages"];

        Check(payload["system"] == "sys", "native: system as top-level field");
        Check(payload.contains("tools") && payload["tools"].size() == 2, "native: tools array sized 2");
        Check(payload["tools"][0]["name"] == "weather" && payload["tools"][0].contains("input_schema"),
              "native: tools[0] has input_schema");

        // user + assistant + user(tool_result*2) + assistant = 4
        Check(m.size() == 4, "native: 4 messages");
        Check(m[0]["role"] == "user", "native: first user");
        Check(m[1]["role"] == "assistant" && m[1]["content"].is_array(), "native: assistant.content is array");
        // assistant.content[0] should be tool_use (no leading text); locate tool_uses
        int toolUses = 0;
        for (const auto& blk : m[1]["content"]) {
            if (blk.value("type", "") == "tool_use") {
                ++toolUses;
                Check(blk.contains("id") && blk.contains("name") && blk.contains("input"),
                      "native: tool_use has id+name+input");
                Check(blk["input"].is_object(), "native: tool_use input is object");
            }
        }
        Check(toolUses == 2, "native: assistant has 2 tool_use blocks");

        Check(m[2]["role"] == "user" && m[2]["content"].is_array(), "native: user follow-up packs tool_results");
        int toolResults = 0;
        for (const auto& blk : m[2]["content"]) {
            if (blk.value("type", "") == "tool_result") {
                ++toolResults;
                Check(blk.contains("tool_use_id") && blk.contains("content"),
                      "native: tool_result has tool_use_id + content");
            }
        }
        Check(toolResults == 2, "native: 2 tool_results folded into single user");

        Check(m[3]["role"] == "assistant", "native: final assistant kept");
    }

    // Fallback mode (no tools field; observations rewritten to user)
    {
        ModelConfig cfg;
        cfg.modelName = "claude-3-7-sonnet";
        cfg.baseUrl = "https://api.anthropic.com";
        cfg.useNativeFunctionCalling = false;
        AnthropicModel model(cfg);

        auto payload = json::parse(model.Format("sys", msgs, {}));
        Check(!payload.contains("tools"), "fallback: no tools field");
        for (const auto& m : payload["messages"]) {
            // No structured tool_use blocks in fallback assistant
            if (m["role"] == "assistant" && m["content"].is_array()) {
                for (const auto& blk : m["content"]) {
                    Check(blk.value("type", "") != "tool_use",
                          "fallback: no tool_use blocks");
                }
            }
        }
    }

    std::cout << "\nAll anthropic_format checks passed.\n";
    return 0;
}


#include <string>
#include <vector>

#include "include/model.h"
#include "include/types.h"
#include "src/models/anthropic_model.h"
#include "third_party/include/nlohmann/json.hpp"
#include "test_runner.h"

using namespace jiuwen;
using json = nlohmann::json;

static std::vector<Message> BuildSampleMessages()
{
    Message u; u.role = "user"; u.content = "Find weather and time";

    Message a1; a1.role = "assistant"; a1.content = "";
    ToolCall tc1; tc1.id = "toolu_a"; tc1.name = "weather"; tc1.argumentsJson = "{\"city\":\"sf\"}";
    ToolCall tc2; tc2.id = "toolu_b"; tc2.name = "now"; tc2.argumentsJson = "{}";
    a1.toolCalls = {tc1, tc2};

    Message t1; t1.role = "tool"; t1.toolCallId = "toolu_a"; t1.toolName = "weather"; t1.content = "22C sunny";
    Message t2; t2.role = "tool"; t2.toolCallId = "toolu_b"; t2.toolName = "now"; t2.content = "2026-06-04";

    Message a2; a2.role = "assistant"; a2.content = "SF is 22C, today is 2026-06-04.";

    return {u, a1, t1, t2, a2};
}

static std::vector<ToolSchema> BuildTwoSchemas()
{
    ToolSchema s1; s1.name = "weather"; s1.description = "Get weather";
    s1.parameters = json{{"type","object"},{"properties",{{"city",{{"type","string"}}}}}};
    ToolSchema s2; s2.name = "now"; s2.description = "Get current time";
    s2.parameters = json{{"type","object"},{"properties", json::object()}};
    return {s1, s2};
}

static int CountBlocksOfType(const json& contentArray, const std::string& type)
{
    int count = 0;
    for (const auto& blk : contentArray) {
        if (blk.value("type", "") == type) ++count;
    }
    return count;
}

TEST(anthropic_format, NativeSystemTopLevel)
{
    ModelConfig cfg;
    cfg.modelName = "claude-3-7-sonnet";
    cfg.baseUrl = "https://api.anthropic.com";
    cfg.useNativeFunctionCalling = true;
    AnthropicModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), BuildTwoSchemas()));
    TestRunner::AssertEq(payload["system"].get<std::string>(), std::string("sys"));
}

TEST(anthropic_format, NativeToolsArray)
{
    ModelConfig cfg;
    cfg.modelName = "claude-3-7-sonnet";
    cfg.baseUrl = "https://api.anthropic.com";
    cfg.useNativeFunctionCalling = true;
    AnthropicModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), BuildTwoSchemas()));
    TestRunner::AssertTrue(payload.contains("tools") && payload["tools"].size() == 2, "tools array sized 2");
    TestRunner::AssertEq(payload["tools"][0]["name"].get<std::string>(), std::string("weather"));
    TestRunner::AssertTrue(payload["tools"][0].contains("input_schema"), "tools[0] has input_schema");
}

TEST(anthropic_format, NativeMessageCount)
{
    ModelConfig cfg;
    cfg.modelName = "claude-3-7-sonnet";
    cfg.baseUrl = "https://api.anthropic.com";
    cfg.useNativeFunctionCalling = true;
    AnthropicModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), BuildTwoSchemas()));
    auto& m = payload["messages"];
    TestRunner::AssertEq(m.size(), size_t(4));
}

TEST(anthropic_format, NativeFirstIsUser)
{
    ModelConfig cfg;
    cfg.modelName = "claude-3-7-sonnet";
    cfg.baseUrl = "https://api.anthropic.com";
    cfg.useNativeFunctionCalling = true;
    AnthropicModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), BuildTwoSchemas()));
    auto& m = payload["messages"];
    TestRunner::AssertEq(m[0]["role"].get<std::string>(), std::string("user"));
}

TEST(anthropic_format, NativeAssistantContentIsArray)
{
    ModelConfig cfg;
    cfg.modelName = "claude-3-7-sonnet";
    cfg.baseUrl = "https://api.anthropic.com";
    cfg.useNativeFunctionCalling = true;
    AnthropicModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), BuildTwoSchemas()));
    auto& m = payload["messages"];
    TestRunner::AssertEq(m[1]["role"].get<std::string>(), std::string("assistant"));
    TestRunner::AssertTrue(m[1]["content"].is_array(), "assistant.content is array");
}

TEST(anthropic_format, NativeToolUseBlocks)
{
    ModelConfig cfg;
    cfg.modelName = "claude-3-7-sonnet";
    cfg.baseUrl = "https://api.anthropic.com";
    cfg.useNativeFunctionCalling = true;
    AnthropicModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), BuildTwoSchemas()));
    auto& m = payload["messages"];
    int toolUses = CountBlocksOfType(m[1]["content"], "tool_use");
    TestRunner::AssertEq(toolUses, 2, "assistant has 2 tool_use blocks");

    for (const auto& blk : m[1]["content"]) {
        if (blk.value("type", "") == "tool_use") {
            TestRunner::AssertTrue(blk.contains("id") && blk.contains("name") && blk.contains("input"),
                                   "tool_use has id+name+input");
            TestRunner::AssertTrue(blk["input"].is_object(), "tool_use input is object");
        }
    }
}

TEST(anthropic_format, NativeToolResultsFoldedIntoUser)
{
    ModelConfig cfg;
    cfg.modelName = "claude-3-7-sonnet";
    cfg.baseUrl = "https://api.anthropic.com";
    cfg.useNativeFunctionCalling = true;
    AnthropicModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), BuildTwoSchemas()));
    auto& m = payload["messages"];
    TestRunner::AssertEq(m[2]["role"].get<std::string>(), std::string("user"));
    TestRunner::AssertTrue(m[2]["content"].is_array(), "user follow-up packs tool_results");

    int toolResults = CountBlocksOfType(m[2]["content"], "tool_result");
    TestRunner::AssertEq(toolResults, 2, "2 tool_results folded into single user");

    for (const auto& blk : m[2]["content"]) {
        if (blk.value("type", "") == "tool_result") {
            TestRunner::AssertTrue(blk.contains("tool_use_id") && blk.contains("content"),
                                   "tool_result has tool_use_id + content");
        }
    }
}

TEST(anthropic_format, NativeFinalAssistant)
{
    ModelConfig cfg;
    cfg.modelName = "claude-3-7-sonnet";
    cfg.baseUrl = "https://api.anthropic.com";
    cfg.useNativeFunctionCalling = true;
    AnthropicModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), BuildTwoSchemas()));
    auto& m = payload["messages"];
    TestRunner::AssertEq(m[3]["role"].get<std::string>(), std::string("assistant"));
}

TEST(anthropic_format, FallbackNoToolsField)
{
    ModelConfig cfg;
    cfg.modelName = "claude-3-7-sonnet";
    cfg.baseUrl = "https://api.anthropic.com";
    cfg.useNativeFunctionCalling = false;
    AnthropicModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), {}));
    TestRunner::AssertFalse(payload.contains("tools"), "fallback: no tools field");
}

TEST(anthropic_format, FallbackNoToolUseBlocks)
{
    ModelConfig cfg;
    cfg.modelName = "claude-3-7-sonnet";
    cfg.baseUrl = "https://api.anthropic.com";
    cfg.useNativeFunctionCalling = false;
    AnthropicModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), {}));
    for (const auto& m : payload["messages"]) {
        if (m["role"] == "assistant" && m["content"].is_array()) {
            int toolUses = CountBlocksOfType(m["content"], "tool_use");
            TestRunner::AssertEq(toolUses, 0, "fallback: no tool_use blocks");
        }
    }
}

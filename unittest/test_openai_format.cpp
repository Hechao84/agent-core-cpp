
#include <cmath>
#include <string>
#include <vector>

#include "include/model.h"
#include "include/types.h"
#include "src/models/openai_model.h"
#include "third_party/include/nlohmann/json.hpp"
#include "test_runner.h"

using namespace jiuwen;
using json = nlohmann::json;

static std::vector<Message> BuildSampleMessages()
{
    Message u1; u1.role = "user"; u1.content = "What's the weather in SF?";

    Message a1; a1.role = "assistant"; a1.content = "";
    ToolCall tc1; tc1.id = "call_s1_1"; tc1.name = "web_search"; tc1.argumentsJson = "{\"q\":\"sf weather\"}";
    a1.toolCalls.push_back(tc1);

    Message t1; t1.role = "tool"; t1.toolCallId = "call_s1_1"; t1.toolName = "web_search"; t1.content = "22C sunny";

    Message a2; a2.role = "assistant"; a2.content = "It's 22C sunny in SF.";

    return {u1, a1, t1, a2};
}

static ToolSchema BuildWebSearchSchema()
{
    ToolSchema schema;
    schema.name = "web_search";
    schema.description = "Search the web";
    schema.parameters = json{{"type","object"},{"properties",{{"q",{{"type","string"}}}}}};
    return schema;
}

TEST(openai_format, NativeMessageCount)
{
    ModelConfig cfg;
    cfg.modelName = "gpt-5";
    cfg.baseUrl = "http://example/v1";
    cfg.useNativeFunctionCalling = true;
    OpenAIModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), {BuildWebSearchSchema()}));
    auto& m = payload["messages"];
    TestRunner::AssertEq(m.size(), size_t(5), "5 wire messages (sys + 4)");
}

TEST(openai_format, NativeSystemFirst)
{
    ModelConfig cfg;
    cfg.modelName = "gpt-5";
    cfg.baseUrl = "http://example/v1";
    cfg.useNativeFunctionCalling = true;
    OpenAIModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), {BuildWebSearchSchema()}));
    auto& m = payload["messages"];
    TestRunner::AssertEq(m[0]["role"].get<std::string>(), std::string("system"));
}

TEST(openai_format, NativeAssistantToolCall)
{
    ModelConfig cfg;
    cfg.modelName = "gpt-5";
    cfg.baseUrl = "http://example/v1";
    cfg.useNativeFunctionCalling = true;
    OpenAIModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), {BuildWebSearchSchema()}));
    auto& m = payload["messages"];
    TestRunner::AssertEq(m[2]["role"].get<std::string>(), std::string("assistant"));
    TestRunner::AssertTrue(m[2]["content"].get<std::string>().empty(), "assistant content empty when tool_calls present");
    TestRunner::AssertTrue(m[2]["tool_calls"].is_array() && m[2]["tool_calls"].size() == 1);
    TestRunner::AssertEq(m[2]["tool_calls"][0]["id"].get<std::string>(), std::string("call_s1_1"));
    TestRunner::AssertEq(m[2]["tool_calls"][0]["type"].get<std::string>(), std::string("function"));
    TestRunner::AssertEq(m[2]["tool_calls"][0]["function"]["name"].get<std::string>(), std::string("web_search"));
    TestRunner::AssertTrue(m[2]["tool_calls"][0]["function"]["arguments"].is_string());
}

TEST(openai_format, NativeToolRole)
{
    ModelConfig cfg;
    cfg.modelName = "gpt-5";
    cfg.baseUrl = "http://example/v1";
    cfg.useNativeFunctionCalling = true;
    OpenAIModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), {BuildWebSearchSchema()}));
    auto& m = payload["messages"];
    TestRunner::AssertEq(m[3]["role"].get<std::string>(), std::string("tool"));
    TestRunner::AssertEq(m[3]["tool_call_id"].get<std::string>(), std::string("call_s1_1"));
    TestRunner::AssertContains(m[3]["content"].get<std::string>(), "22C");
}

TEST(openai_format, NativeFinalAssistant)
{
    ModelConfig cfg;
    cfg.modelName = "gpt-5";
    cfg.baseUrl = "http://example/v1";
    cfg.useNativeFunctionCalling = true;
    OpenAIModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), {BuildWebSearchSchema()}));
    auto& m = payload["messages"];
    TestRunner::AssertEq(m[4]["role"].get<std::string>(), std::string("assistant"));
    TestRunner::AssertEq(m[4]["content"].get<std::string>(), std::string("It's 22C sunny in SF."));
}

TEST(openai_format, NativeToolsArray)
{
    ModelConfig cfg;
    cfg.modelName = "gpt-5";
    cfg.baseUrl = "http://example/v1";
    cfg.useNativeFunctionCalling = true;
    OpenAIModel model(cfg);

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), {BuildWebSearchSchema()}));
    TestRunner::AssertTrue(payload.contains("tools") && payload["tools"].is_array());
    TestRunner::AssertEq(payload["tools"][0]["type"].get<std::string>(), std::string("function"));
    TestRunner::AssertEq(payload["tools"][0]["function"]["name"].get<std::string>(), std::string("web_search"));
}

TEST(openai_format, FallbackNoToolsField)
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

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), {schema}));
    TestRunner::AssertFalse(payload.contains("tools"), "fallback: no tools field on payload");
}

TEST(openai_format, FallbackNoRawToolRole)
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

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), {schema}));
    auto& m = payload["messages"];
    for (size_t i = 0; i < m.size(); ++i) {
        TestRunner::AssertFalse(m[i]["role"] == "tool", "fallback: no raw tool role at i=" + std::to_string(i));
    }
}

TEST(openai_format, FallbackEndsWithUserOrAssistant)
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

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), {schema}));
    auto& m = payload["messages"];
    std::string lastRole = m.back()["role"].get<std::string>();
    TestRunner::AssertTrue(lastRole == "user" || lastRole == "assistant", "fallback: ends in user or assistant");
}

TEST(openai_format, FallbackToolCatalogueInSystem)
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

    auto payload = json::parse(model.Format("sys", BuildSampleMessages(), {schema}));
    auto& m = payload["messages"];
    TestRunner::AssertEq(m[0]["role"].get<std::string>(), std::string("system"));
    TestRunner::AssertContains(m[0]["content"].get<std::string>(), "web_search");
}

TEST(openai_extra_params, MaxTokensForwarded)
{
    ModelConfig cfg;
    cfg.modelName = "ark-code-latest";
    cfg.baseUrl = "https://example/v1";
    cfg.useNativeFunctionCalling = true;
    cfg.extraParams.Set("max_tokens", 8192);
    OpenAIModel model(cfg);

    Message u; u.role = "user"; u.content = "ping";
    auto payload = json::parse(model.Format("sys", {u}, {}));
    TestRunner::AssertEq(payload.value("max_tokens", 0), 8192);
}

TEST(openai_extra_params, TemperatureForwarded)
{
    ModelConfig cfg;
    cfg.modelName = "ark-code-latest";
    cfg.baseUrl = "https://example/v1";
    cfg.useNativeFunctionCalling = true;
    cfg.extraParams.Set("temperature", 0.2f);
    OpenAIModel model(cfg);

    Message u; u.role = "user"; u.content = "ping";
    auto payload = json::parse(model.Format("sys", {u}, {}));
    float val = payload.value("temperature", 0.0f);
    TestRunner::AssertTrue(std::abs(val - 0.2f) < 1e-5f, "temperature forwarded");
}

TEST(openai_extra_params, TopPForwarded)
{
    ModelConfig cfg;
    cfg.modelName = "ark-code-latest";
    cfg.baseUrl = "https://example/v1";
    cfg.useNativeFunctionCalling = true;
    cfg.extraParams.Set("top_p", 0.9f);
    OpenAIModel model(cfg);

    Message u; u.role = "user"; u.content = "ping";
    auto payload = json::parse(model.Format("sys", {u}, {}));
    float val = payload.value("top_p", 0.0f);
    TestRunner::AssertTrue(std::abs(val - 0.9f) < 1e-5f, "top_p forwarded");
}

TEST(openai_extra_params, PresencePenaltyForwarded)
{
    ModelConfig cfg;
    cfg.modelName = "ark-code-latest";
    cfg.baseUrl = "https://example/v1";
    cfg.useNativeFunctionCalling = true;
    cfg.extraParams.Set("presence_penalty", 0.1f);
    OpenAIModel model(cfg);

    Message u; u.role = "user"; u.content = "ping";
    auto payload = json::parse(model.Format("sys", {u}, {}));
    float val = payload.value("presence_penalty", 0.0f);
    TestRunner::AssertTrue(std::abs(val - 0.1f) < 1e-5f, "presence_penalty forwarded");
}

TEST(openai_extra_params, FrequencyPenaltyForwarded)
{
    ModelConfig cfg;
    cfg.modelName = "ark-code-latest";
    cfg.baseUrl = "https://example/v1";
    cfg.useNativeFunctionCalling = true;
    cfg.extraParams.Set("frequency_penalty", 0.05f);
    OpenAIModel model(cfg);

    Message u; u.role = "user"; u.content = "ping";
    auto payload = json::parse(model.Format("sys", {u}, {}));
    float val = payload.value("frequency_penalty", 0.0f);
    TestRunner::AssertTrue(std::abs(val - 0.05f) < 1e-5f, "frequency_penalty forwarded");
}

TEST(openai_extra_params, SeedForwarded)
{
    ModelConfig cfg;
    cfg.modelName = "ark-code-latest";
    cfg.baseUrl = "https://example/v1";
    cfg.useNativeFunctionCalling = true;
    cfg.extraParams.Set("seed", 42);
    OpenAIModel model(cfg);

    Message u; u.role = "user"; u.content = "ping";
    auto payload = json::parse(model.Format("sys", {u}, {}));
    TestRunner::AssertEq(payload.value("seed", 0), 42);
}

TEST(openai_extra_params, AbsentKeysNotInPayload)
{
    ModelConfig cfg;
    cfg.modelName = "x";
    cfg.baseUrl = "http://x";
    OpenAIModel model(cfg);

    Message u; u.role = "user"; u.content = "ping";
    auto payload = json::parse(model.Format("sys", {u}, {}));
    TestRunner::AssertFalse(payload.contains("max_tokens"), "absent: no max_tokens");
    TestRunner::AssertFalse(payload.contains("temperature"), "absent: no temperature");
    TestRunner::AssertFalse(payload.contains("top_p"), "absent: no top_p");
    TestRunner::AssertFalse(payload.contains("seed"), "absent: no seed");
}

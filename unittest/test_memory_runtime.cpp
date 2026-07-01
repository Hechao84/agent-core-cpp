#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "include/model.h"
#include "include/resource_manager.h"
#include "src/tools/builtin_tools/memory_read_payload_tool.h"
#include "test_runner.h"
#include "third_party/include/nlohmann/json.hpp"

#ifdef JIUWEN_ENABLE_MEMORY_BUILTIN
#include "src/memory/builtin_memory_runtime.h"
#endif

namespace fs = std::filesystem;
using namespace jiuwen;

class MemoryRuntimeTestModel : public Model
{
public:
    explicit MemoryRuntimeTestModel(std::string response)
        : Model(ModelConfig()), response_(std::move(response))
    {
    }

    std::string Format(const std::string& systemPrompt,
                       const std::vector<Message>& messages,
                       const std::vector<ToolSchema>& tools) override
    {
        (void)tools;
        formatted_ = systemPrompt;
        if (!messages.empty()) {
            formatted_ += "\n" + messages.front().content;
        }
        return formatted_;
    }

    ModelResponse Invoke(const std::string& formattedInput,
                         std::function<void(const std::string&)> onChunk,
                         std::function<bool()> /*shouldCancel*/) override
    {
        (void)formattedInput;
        (void)onChunk;
        ModelResponse response;
        response.content = response_;
        response.isFinished = true;
        response.finishReason = "stop";
        return response;
    }

    std::string formatted_;

private:
    std::string response_;
};

// Wraps a test Model as a MemoryModelClient for Consolidate, mirroring the
// HostMemoryModelClient bridge the framework uses internally.
class TestMemoryModelClient : public MemoryModelClient
{
public:
    explicit TestMemoryModelClient(Model* model) : model_(model) {}

    MemoryModelResult GenerateMemoryUpdate(const std::string& prompt) override
    {
        MemoryModelResult result;
        Message userMsg;
        userMsg.role = "user";
        userMsg.content = prompt;
        std::string formatted = model_->Format("system", {userMsg}, {});
        ModelResponse response = model_->Invoke(formatted, nullptr);
        result.text = response.content;
        result.httpStatus = 200;
        if (response.content.empty()) {
            result.errorCode = "empty_response";
            result.errorMessage = "Model returned empty content";
        }
        return result;
    }

private:
    Model* model_;
};

#ifdef JIUWEN_ENABLE_MEMORY_BUILTIN
TEST(memory_runtime, ResourceManagerRegistersBuiltinCompat)
{
    auto& rm = ResourceManager::GetInstance();
    TestRunner::AssertTrue(rm.HasMemoryRuntime("builtin.compat"));

    MemoryConfig config;
    config.provider = "builtin.compat";
    auto runtime = rm.CreateMemoryRuntime(config);
    TestRunner::AssertTrue(runtime != nullptr);
}
#endif // JIUWEN_ENABLE_MEMORY_BUILTIN

TEST(memory_runtime, ResourceManagerRegistersHttpServerRuntime)
{
    MemoryConfig config;
    config.enabled = true;
    config.mode = "server";
    config.provider = "http.server";
    config.serverUrl = "http://127.0.0.1:8090";
    auto runtime = ResourceManager::GetInstance().CreateMemoryRuntime(config);
    TestRunner::AssertTrue(runtime != nullptr);
    TestRunner::AssertEq(runtime->GetConfig().serverUrl, std::string("http://127.0.0.1:8090"));
}

#ifdef JIUWEN_ENABLE_MEMORY_BUILTIN
TEST(memory_runtime, AppendEventUpdatesStats)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_stats_test";
    fs::remove_all(base);

    MemoryConfig config;
    config.dataPath = base.string();
    BuiltinMemoryRuntime runtime(config);

    MemoryEvent event;
    event.type = MemoryEventType::MESSAGE_APPENDED;
    event.agentId = "agent";
    event.sessionId = "session";
    event.role = "user";
    event.content = "hello";

    TestRunner::AssertTrue(runtime.AppendEvent(event));
    MemoryStats stats = runtime.GetStats();
    TestRunner::AssertEq(stats.events, 1);

    fs::remove_all(base);
}

TEST(memory_runtime, WritePayloadOffloadsLargeContent)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_payload_offload_test";
    fs::remove_all(base);

    MemoryConfig config;
    config.dataPath = base.string();
    config.enablePayloadOffload = true;
    config.offloadToolResultChars = 8;
    BuiltinMemoryRuntime runtime(config);

    MemoryPayloadWriteRequest request;
    request.agentId = "agent";
    request.sessionId = "session";
    request.content = "large tool output";
    request.contentType = "tool_result";
    request.toolCallId = "call_1";
    request.toolName = "grep";

    MemoryPayloadWriteResult result = runtime.WritePayload(request);
    TestRunner::AssertTrue(result.offloaded);
    TestRunner::AssertContains(result.replacementContent, "memory-ref");
    TestRunner::AssertContains(result.replacementContent, "Tool result offloaded from grep");
    TestRunner::AssertContains(result.payload.uri, "file://");
    TestRunner::AssertEq(runtime.ReadPayload(result.payload.uri), std::string("large tool output"));

    fs::remove_all(base);
}

TEST(memory_runtime, BuildContextIncludesOffloadedPayloadOverview)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_context_payload_test";
    fs::remove_all(base);

    MemoryConfig config;
    config.dataPath = base.string();
    config.enablePayloadOffload = true;
    config.offloadToolResultChars = 8;
    BuiltinMemoryRuntime runtime(config);

    MemoryPayloadWriteRequest writeRequest;
    writeRequest.sessionId = "session";
    writeRequest.content = "large tool output";
    writeRequest.contentType = "tool_result";
    writeRequest.toolCallId = "call_1";
    writeRequest.toolName = "grep";
    runtime.WritePayload(writeRequest);

    MemoryContextRequest contextRequest;
    contextRequest.sessionId = "session";
    MemoryContextPackage context = runtime.BuildContext(contextRequest);

    TestRunner::AssertContains(context.memoryText, "## Offloaded Payloads");
    TestRunner::AssertContains(context.memoryText, "grep");
    TestRunner::AssertContains(context.memoryText, "Tool result offloaded from grep");
    TestRunner::AssertEq(context.payloadRefs.size(), size_t(1));
    TestRunner::AssertEq(context.metadata["payload_count"].get<int>(), 1);

    fs::remove_all(base);
}

TEST(memory_runtime, MemoryReadPayloadToolReadsOffloadedContent)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_read_payload_tool_test";
    fs::remove_all(base);

    MemoryConfig config;
    config.dataPath = base.string();
    config.enablePayloadOffload = true;
    config.offloadToolResultChars = 8;
    BuiltinMemoryRuntime runtime(config);

    MemoryPayloadWriteRequest request;
    request.sessionId = "session";
    request.content = "large tool output";
    request.contentType = "tool_result";
    request.toolCallId = "call_1";
    request.toolName = "grep";
    MemoryPayloadWriteResult payload = runtime.WritePayload(request);

    MemoryReadPayloadTool tool(&runtime);
    nlohmann::json input;
    input["ref"] = payload.payload.uri;
    nlohmann::json result = nlohmann::json::parse(tool.Invoke(input.dump()));

    TestRunner::AssertTrue(result.value("ok", false));
    TestRunner::AssertEq(result.value("content", std::string()), std::string("large tool output"));

    fs::remove_all(base);
}

TEST(memory_runtime, WritePayloadKeepsSmallContent)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_payload_small_test";
    fs::remove_all(base);

    MemoryConfig config;
    config.dataPath = base.string();
    config.enablePayloadOffload = true;
    config.offloadToolResultChars = 100;
    BuiltinMemoryRuntime runtime(config);

    MemoryPayloadWriteRequest request;
    request.content = "small";

    MemoryPayloadWriteResult result = runtime.WritePayload(request);
    TestRunner::AssertFalse(result.offloaded);
    TestRunner::AssertEq(result.replacementContent, std::string("small"));

    fs::remove_all(base);
}

TEST(memory_runtime, PersistsEventsAndPayloadsToSqliteStats)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_sqlite_store_test";
    fs::remove_all(base);

    MemoryConfig config;
    config.dataPath = base.string();
    config.enablePayloadOffload = true;
    config.offloadToolResultChars = 8;
    BuiltinMemoryRuntime runtime(config);

    MemoryEvent event;
    event.type = MemoryEventType::MESSAGE_APPENDED;
    event.agentId = "agent";
    event.sessionId = "session";
    event.role = "user";
    event.content = "hello";
    runtime.AppendEvent(event);

    MemoryPayloadWriteRequest request;
    request.sessionId = "session";
    request.content = "large tool output";
    request.contentType = "tool_result";
    request.toolCallId = "call_1";
    request.toolName = "grep";
    runtime.WritePayload(request);

    MemoryStats stats = runtime.GetStats();
    TestRunner::AssertEq(stats.events, 1);
    TestRunner::AssertEq(stats.payloads, 1);
    TestRunner::AssertEq(stats.summaries, 0);
    TestRunner::AssertEq(stats.entities, 0);
    TestRunner::AssertEq(stats.relations, 0);
    TestRunner::AssertTrue(fs::exists(base / "memory_runtime" / "memory.db"));

    fs::remove_all(base);
}

TEST(memory_runtime, ConsolidateWritesSessionSummary)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_consolidate_test";
    fs::remove_all(base);

    MemoryConfig config;
    config.dataPath = base.string();
    BuiltinMemoryRuntime runtime(config);

    MemoryEvent event;
    event.type = MemoryEventType::MESSAGE_APPENDED;
    event.agentId = "agent";
    event.sessionId = "session";
    event.role = "user";
    event.content = "remember this important fact";
    TestRunner::AssertTrue(runtime.AppendEvent(event));

    MemoryConsolidationRequest request;
    request.agentId = "agent";
    request.sessionId = "session";
    request.maxEvents = 10;
    TestRunner::AssertTrue(runtime.Consolidate(request, nullptr));

    MemoryContextRequest contextRequest;
    contextRequest.agentId = "agent";
    contextRequest.sessionId = "session";
    MemoryContextPackage context = runtime.BuildContext(contextRequest);
    TestRunner::AssertContains(context.memoryText, "## Long-term Summaries");
    TestRunner::AssertContains(context.memoryText, "remember this important fact");
    TestRunner::AssertContains(context.memoryText, "[session]");

    MemoryStats stats = runtime.GetStats();
    TestRunner::AssertEq(stats.summaries, 1);

    fs::remove_all(base);
}

TEST(memory_runtime, ConsolidateRuntimeDecidesOverload)
{
    // The no-arg Consolidate(request) overload lets the runtime decide the
    // model source. With no model configured it falls back to rule-based
    // extraction, mirroring the agent-memory-cpp runtime-decides semantics.
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_consolidate_noarg_test";
    fs::remove_all(base);

    MemoryConfig config;
    config.dataPath = base.string();
    BuiltinMemoryRuntime runtime(config);

    MemoryEvent event;
    event.type = MemoryEventType::MESSAGE_APPENDED;
    event.agentId = "agent";
    event.sessionId = "session";
    event.role = "user";
    event.content = "remember this important fact";
    TestRunner::AssertTrue(runtime.AppendEvent(event));

    MemoryConsolidationRequest request;
    request.agentId = "agent";
    request.sessionId = "session";
    request.maxEvents = 10;
    TestRunner::AssertTrue(runtime.Consolidate(request));

    MemoryContextRequest contextRequest;
    contextRequest.agentId = "agent";
    contextRequest.sessionId = "session";
    MemoryContextPackage context = runtime.BuildContext(contextRequest);
    TestRunner::AssertContains(context.memoryText, "## Long-term Summaries");
    TestRunner::AssertContains(context.memoryText, "remember this important fact");

    fs::remove_all(base);
}

TEST(memory_runtime, ConsolidateExtractsTopicPreferenceEntityRelation)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_processor_test";
    fs::remove_all(base);

    MemoryConfig config;
    config.dataPath = base.string();
    BuiltinMemoryRuntime runtime(config);

    MemoryEvent event;
    event.type = MemoryEventType::MESSAGE_APPENDED;
    event.agentId = "agent";
    event.sessionId = "session";
    event.role = "user";
    event.content = "I prefer testing code carefully in this project";
    TestRunner::AssertTrue(runtime.AppendEvent(event));

    MemoryConsolidationRequest request;
    request.agentId = "agent";
    request.sessionId = "session";
    TestRunner::AssertTrue(runtime.Consolidate(request, nullptr));

    MemoryContextRequest contextRequest;
    contextRequest.agentId = "agent";
    contextRequest.sessionId = "session";
    MemoryContextPackage context = runtime.BuildContext(contextRequest);

    TestRunner::AssertContains(context.memoryText, "Discussed topic: project");
    TestRunner::AssertContains(context.memoryText, "entity:preference.user");
    TestRunner::AssertContains(context.memoryText, "entity:user has_preference entity:preference.user");

    MemoryStats stats = runtime.GetStats();
    TestRunner::AssertTrue(stats.summaries >= 2);
    TestRunner::AssertTrue(stats.entities >= 2);
    TestRunner::AssertTrue(stats.relations >= 1);

    fs::remove_all(base);
}

TEST(memory_runtime, ConsolidateUsesLlmProcessorWhenModelProvided)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_llm_processor_test";
    fs::remove_all(base);

    MemoryConfig config;
    config.dataPath = base.string();
    BuiltinMemoryRuntime runtime(config);

    MemoryEvent event;
    event.type = MemoryEventType::MESSAGE_APPENDED;
    event.agentId = "agent";
    event.sessionId = "session";
    event.role = "user";
    event.content = "I prefer concise answers for this project";
    TestRunner::AssertTrue(runtime.AppendEvent(event));

    std::string response = R"({
        "topicSummaries": ["User discussed answer style"],
        "profileSummaries": ["User prefers concise answers"],
        "entities": [{
            "id": "entity:preference.concise_answers",
            "entityType": "preference",
            "name": "Concise answers",
            "summary": "User prefers concise answers",
            "confidence": 0.9,
            "sourceRefs": ["event://1"]
        }],
        "relations": [{
            "fromEntityId": "entity:user",
            "relationType": "prefers",
            "toEntityId": "entity:preference.concise_answers",
            "confidence": 0.85,
            "sourceRefs": ["event://1"]
        }]
    })";
    MemoryRuntimeTestModel model(response);

    MemoryConsolidationRequest request;
    request.agentId = "agent";
    request.sessionId = "session";
    TestMemoryModelClient client(&model);
    TestRunner::AssertTrue(runtime.Consolidate(request, &client));

    MemoryContextRequest contextRequest;
    contextRequest.agentId = "agent";
    contextRequest.sessionId = "session";
    MemoryContextPackage context = runtime.BuildContext(contextRequest);

    TestRunner::AssertContains(context.memoryText, "User discussed answer style");
    TestRunner::AssertContains(context.memoryText, "User prefers concise answers");
    TestRunner::AssertContains(context.memoryText, "[profile]");
    TestRunner::AssertContains(context.memoryText, "Concise answers");

    fs::remove_all(base);
}

TEST(memory_runtime, ConsolidateFallsBackWhenLlmJsonInvalid)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_llm_fallback_test";
    fs::remove_all(base);

    MemoryConfig config;
    config.dataPath = base.string();
    BuiltinMemoryRuntime runtime(config);

    MemoryEvent event;
    event.type = MemoryEventType::MESSAGE_APPENDED;
    event.agentId = "agent";
    event.sessionId = "session";
    event.role = "user";
    event.content = "I prefer testing code carefully in this project";
    TestRunner::AssertTrue(runtime.AppendEvent(event));

    MemoryRuntimeTestModel model("not json");

    MemoryConsolidationRequest request;
    request.agentId = "agent";
    request.sessionId = "session";
    TestMemoryModelClient client(&model);
    TestRunner::AssertTrue(runtime.Consolidate(request, &client));

    MemoryContextRequest contextRequest;
    contextRequest.agentId = "agent";
    contextRequest.sessionId = "session";
    MemoryContextPackage context = runtime.BuildContext(contextRequest);

    TestRunner::AssertContains(context.memoryText, "Discussed topic: project");
    TestRunner::AssertContains(context.memoryText, "entity:preference.user");
    TestRunner::AssertContains(context.memoryText, "entity:user has_preference entity:preference.user");

    fs::remove_all(base);
}

TEST(memory_runtime, SupersedesRelationMarksOldEntityObsolete)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_supersedes_test";
    fs::remove_all(base);

    MemoryConfig config;
    config.dataPath = base.string();
    BuiltinMemoryRuntime runtime(config);

    MemoryEvent event1;
    event1.type = MemoryEventType::MESSAGE_APPENDED;
    event1.agentId = "agent";
    event1.sessionId = "session";
    event1.role = "user";
    event1.content = "I prefer verbose answers";
    runtime.AppendEvent(event1);

    MemoryEvent event2;
    event2.type = MemoryEventType::MESSAGE_APPENDED;
    event2.agentId = "agent";
    event2.sessionId = "session";
    event2.role = "user";
    event2.content = "Actually I now prefer concise answers";
    runtime.AppendEvent(event2);

    std::string response = R"({
        "topicSummaries": [],
        "profileSummaries": ["User prefers concise answers"],
        "entities": [{
            "id": "entity:preference.verbose_answers",
            "entityType": "preference",
            "name": "Verbose answers",
            "summary": "User previously preferred verbose answers",
            "confidence": 0.7,
            "sourceRefs": ["event://1"]
        }, {
            "id": "entity:preference.concise_answers",
            "entityType": "preference",
            "name": "Concise answers",
            "summary": "User prefers concise answers",
            "confidence": 0.9,
            "sourceRefs": ["event://2"]
        }],
        "relations": [{
            "fromEntityId": "entity:preference.concise_answers",
            "relationType": "supersedes",
            "toEntityId": "entity:preference.verbose_answers",
            "confidence": 0.85,
            "sourceRefs": ["event://2"]
        }]
    })";
    MemoryRuntimeTestModel model(response);

    MemoryConsolidationRequest request;
    request.agentId = "agent";
    request.sessionId = "session";
    TestMemoryModelClient client(&model);
    TestRunner::AssertTrue(runtime.Consolidate(request, &client));

    MemoryContextRequest contextRequest;
    contextRequest.agentId = "agent";
    contextRequest.sessionId = "session";
    MemoryContextPackage context = runtime.BuildContext(contextRequest);

    TestRunner::AssertContains(context.memoryText, "entity:preference.concise_answers");
    TestRunner::AssertContains(context.memoryText, "User prefers concise answers");
    TestRunner::AssertTrue(context.memoryText.find("User previously preferred verbose answers") == std::string::npos);

    fs::remove_all(base);
}
#endif // JIUWEN_ENABLE_MEMORY_BUILTIN



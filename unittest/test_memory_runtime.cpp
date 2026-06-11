#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "include/model.h"
#include "include/resource_manager.h"
#include "src/memory/builtin_memory_runtime.h"
#include "src/memory/memory_sqlite_store.h"
#include "src/tools/builtin_tools/memory_read_payload_tool.h"
#include "test_runner.h"
#include "third_party/include/nlohmann/json.hpp"

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
                         std::function<void(const std::string&)> onChunk) override
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

TEST(memory_runtime, ResourceManagerRegistersBuiltinCompat)
{
    auto& rm = ResourceManager::GetInstance();
    TestRunner::AssertTrue(rm.HasMemoryRuntime("builtin.compat"));

    MemoryConfig config;
    config.provider = "builtin.compat";
    auto runtime = rm.CreateMemoryRuntime(config);
    TestRunner::AssertTrue(runtime != nullptr);
}

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

TEST(memory_runtime, BuildContextLoadsLegacyMemoryFiles)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_runtime_test";
    fs::remove_all(base);
    fs::create_directories(base / "memory");

    {
        std::ofstream out(base / "memory" / "MEMORY.md");
        out << "Project uses C++17.";
    }
    {
        std::ofstream out(base / "SOUL.md");
        out << "Be concise.";
    }
    {
        std::ofstream out(base / "USER.md");
        out << "User prefers direct answers.";
    }

    MemoryConfig config;
    config.provider = "builtin.compat";
    config.dataPath = base.string();
    BuiltinMemoryRuntime runtime(config);

    MemoryContextRequest request;
    request.agentId = "agent";
    request.sessionId = "session";
    auto context = runtime.BuildContext(request);

    TestRunner::AssertContains(context.memoryText, "## MEMORY");
    TestRunner::AssertContains(context.memoryText, "Project uses C++17.");
    TestRunner::AssertContains(context.memoryText, "## SOUL");
    TestRunner::AssertContains(context.memoryText, "Be concise.");
    TestRunner::AssertContains(context.memoryText, "## USER");
    TestRunner::AssertContains(context.memoryText, "User prefers direct answers.");

    fs::remove_all(base);
}

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

TEST(memory_runtime, ReadPayloadSupportsFileRefs)
{
    fs::path path = fs::temp_directory_path() / "jiuwen_memory_payload.txt";
    {
        std::ofstream out(path);
        out << "payload content";
    }

    MemoryConfig config;
    BuiltinMemoryRuntime runtime(config);

    std::string content = runtime.ReadPayload("file://" + path.string());
    TestRunner::AssertEq(content, std::string("payload content"));

    fs::remove(path);
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
    TestRunner::AssertContains(result.payload.ref, "file://");
    TestRunner::AssertEq(runtime.ReadPayload(result.payload.ref), std::string("large tool output"));

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
    TestRunner::AssertEq(context.metadata["payload_count"], std::string("1"));

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
    input["ref"] = payload.payload.ref;
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

TEST(memory_runtime, BuildContextIncludesStructuredLongTermMemory)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_context_structured_test";
    fs::remove_all(base);
    fs::create_directories(base / "memory_runtime");

    {
        MemorySqliteStore store((base / "memory_runtime" / "memory.db").string());
        TestRunner::AssertTrue(store.Initialize());
        TestRunner::AssertTrue(store.SaveSummary("agent", "session", "session", "topic", "session summary", 0.8F,
                                                 {"event://1"}));
        MemoryEntity entity;
        entity.id = "entity:user";
        entity.type = "user";
        entity.name = "User";
        entity.summary = "Primary user";
        entity.confidence = 0.9F;
        entity.sourceRefs = {"summary://session/1"};
        TestRunner::AssertTrue(store.SaveEntity(entity));
        MemoryRelation relation;
        relation.fromEntity = "entity:user";
        relation.relation = "prefers";
        relation.toEntity = "entity:style";
        relation.confidence = 0.7F;
        relation.sourceRefs = {"entity://user"};
        TestRunner::AssertTrue(store.SaveRelation(relation));
    }

    MemoryConfig config;
    config.dataPath = base.string();
    BuiltinMemoryRuntime runtime(config);
    MemoryContextRequest request;
    MemoryContextPackage context = runtime.BuildContext(request);

    TestRunner::AssertContains(context.memoryText, "## Long-term Summaries");
    TestRunner::AssertContains(context.memoryText, "session summary");
    TestRunner::AssertContains(context.memoryText, "## Memory Entities");
    TestRunner::AssertContains(context.memoryText, "Primary user");
    TestRunner::AssertContains(context.memoryText, "## Memory Relations");
    TestRunner::AssertContains(context.memoryText, "entity:user prefers entity:style");
    TestRunner::AssertContains(context.memoryText, "event://1");
    TestRunner::AssertContains(context.memoryText, "summary://session/1");
    TestRunner::AssertContains(context.memoryText, "entity://user");

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
    TestRunner::AssertTrue(runtime.Consolidate(request));

    MemoryContextRequest contextRequest;
    contextRequest.agentId = "agent";
    contextRequest.sessionId = "session";
    MemoryContextPackage context = runtime.BuildContext(contextRequest);
    TestRunner::AssertContains(context.memoryText, "## Long-term Summaries");
    TestRunner::AssertContains(context.memoryText, "remember this important fact");
    TestRunner::AssertContains(context.memoryText, "session://session");

    MemoryStats stats = runtime.GetStats();
    TestRunner::AssertEq(stats.summaries, 1);

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
    TestRunner::AssertTrue(runtime.Consolidate(request));

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
            "type": "preference",
            "name": "Concise answers",
            "summary": "User prefers concise answers",
            "confidence": 0.9,
            "sourceRefs": ["event://1"]
        }],
        "relations": [{
            "fromEntity": "entity:user",
            "relation": "prefers",
            "toEntity": "entity:preference.concise_answers",
            "confidence": 0.85,
            "sourceRefs": ["event://1"]
        }]
    })";
    MemoryRuntimeTestModel model(response);

    MemoryConsolidationRequest request;
    request.agentId = "agent";
    request.sessionId = "session";
    TestRunner::AssertTrue(runtime.Consolidate(request, &model));

    MemoryContextRequest contextRequest;
    contextRequest.agentId = "agent";
    contextRequest.sessionId = "session";
    MemoryContextPackage context = runtime.BuildContext(contextRequest);

    TestRunner::AssertContains(context.memoryText, "User discussed answer style");
    TestRunner::AssertContains(context.memoryText, "User prefers concise answers");
    TestRunner::AssertContains(context.memoryText, "entity:preference.concise_answers");
    TestRunner::AssertContains(context.memoryText, "entity:user prefers entity:preference.concise_answers");

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
    TestRunner::AssertTrue(runtime.Consolidate(request, &model));

    MemoryContextRequest contextRequest;
    contextRequest.agentId = "agent";
    contextRequest.sessionId = "session";
    MemoryContextPackage context = runtime.BuildContext(contextRequest);

    TestRunner::AssertContains(context.memoryText, "Discussed topic: project");
    TestRunner::AssertContains(context.memoryText, "entity:preference.user");
    TestRunner::AssertContains(context.memoryText, "entity:user has_preference entity:preference.user");

    fs::remove_all(base);
}

TEST(memory_runtime, MemorySqliteStorePersistsSummaryEntityRelation)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_sqlite_structured_test";
    fs::remove_all(base);
    fs::create_directories(base);

    MemorySqliteStore store((base / "memory.db").string());
    TestRunner::AssertTrue(store.Initialize());
    TestRunner::AssertTrue(store.SaveSummary("agent", "session", "session", "topic", "summary", 0.8F));

    MemoryEntity entity;
    entity.id = "entity:user";
    entity.type = "user";
    entity.name = "User";
    entity.summary = "Primary user";
    entity.confidence = 0.9F;
    TestRunner::AssertTrue(store.SaveEntity(entity));

    MemoryRelation relation;
    relation.fromEntity = "entity:user";
    relation.relation = "prefers";
    relation.toEntity = "entity:style";
    relation.confidence = 0.7F;
    TestRunner::AssertTrue(store.SaveRelation(relation));

    TestRunner::AssertEq(store.CountRows("memory_summaries"), 1);
    TestRunner::AssertEq(store.CountRows("memory_entities"), 1);
    TestRunner::AssertEq(store.CountRows("memory_relations"), 1);

    fs::remove_all(base);
}

TEST(memory_runtime, AppendEventPersistsLegacyHistoryJsonl)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_runtime_history_test";
    fs::remove_all(base);

    MemoryConfig config;
    config.dataPath = base.string();
    BuiltinMemoryRuntime runtime(config);

    MemoryEvent event;
    event.type = MemoryEventType::MESSAGE_APPENDED;
    event.agentId = "agent";
    event.sessionId = "session";
    event.role = "tool";
    event.content = "tool output";
    event.toolCallId = "call_1";
    event.toolName = "grep";

    TestRunner::AssertTrue(runtime.AppendEvent(event));

    fs::path historyPath = base / "memory" / "history.jsonl";
    std::ifstream historyFile(historyPath);
    TestRunner::AssertTrue(historyFile.is_open());

    std::string line;
    std::getline(historyFile, line);
    nlohmann::json entry = nlohmann::json::parse(line);
    TestRunner::AssertEq(entry.value("cursor", 0), 1);
    TestRunner::AssertEq(entry.value("session_id", std::string()), std::string("session"));
    TestRunner::AssertEq(entry.value("role", std::string()), std::string("tool"));
    TestRunner::AssertEq(entry.value("content", std::string()), std::string("tool output"));
    TestRunner::AssertEq(entry.value("tool_call_id", std::string()), std::string("call_1"));
    TestRunner::AssertEq(entry.value("tool_name", std::string()), std::string("grep"));

    std::ifstream cursorFile(base / "memory" / ".cursor");
    TestRunner::AssertTrue(cursorFile.is_open());
    std::string cursor;
    std::getline(cursorFile, cursor);
    TestRunner::AssertEq(cursor, std::string("1"));

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
            "type": "preference",
            "name": "Verbose answers",
            "summary": "User previously preferred verbose answers",
            "confidence": 0.7,
            "sourceRefs": ["event://1"]
        }, {
            "id": "entity:preference.concise_answers",
            "type": "preference",
            "name": "Concise answers",
            "summary": "User prefers concise answers",
            "confidence": 0.9,
            "sourceRefs": ["event://2"]
        }],
        "relations": [{
            "fromEntity": "entity:preference.concise_answers",
            "relation": "supersedes",
            "toEntity": "entity:preference.verbose_answers",
            "confidence": 0.85,
            "sourceRefs": ["event://2"]
        }]
    })";
    MemoryRuntimeTestModel model(response);

    MemoryConsolidationRequest request;
    request.agentId = "agent";
    request.sessionId = "session";
    TestRunner::AssertTrue(runtime.Consolidate(request, &model));

    MemoryContextRequest contextRequest;
    contextRequest.agentId = "agent";
    contextRequest.sessionId = "session";
    MemoryContextPackage context = runtime.BuildContext(contextRequest);

    TestRunner::AssertContains(context.memoryText, "entity:preference.concise_answers");
    TestRunner::AssertContains(context.memoryText, "User prefers concise answers");
    TestRunner::AssertTrue(context.memoryText.find("User previously preferred verbose answers") == std::string::npos);

    fs::remove_all(base);
}

TEST(memory_runtime, EntityUpdatePreservesPreviousSummary)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_entity_update_test";
    fs::remove_all(base);
    fs::create_directories(base / "memory_runtime");

    {
        MemorySqliteStore store((base / "memory_runtime" / "memory.db").string());
        TestRunner::AssertTrue(store.Initialize());

        MemoryEntity entity1;
        entity1.id = "entity:preference.style";
        entity1.type = "preference";
        entity1.name = "Style preference";
        entity1.summary = "User prefers verbose output";
        entity1.confidence = 0.7F;
        entity1.sourceRefs = {"event://1"};
        TestRunner::AssertTrue(store.SaveEntity(entity1));

        MemoryEntity entity2;
        entity2.id = "entity:preference.style";
        entity2.type = "preference";
        entity2.name = "Style preference";
        entity2.summary = "User prefers concise output";
        entity2.confidence = 0.9F;
        entity2.sourceRefs = {"event://2"};
        TestRunner::AssertTrue(store.SaveEntity(entity2));
    }

    {
        MemorySqliteStore store((base / "memory_runtime" / "memory.db").string());
        TestRunner::AssertTrue(store.Initialize());
        std::string text = store.LoadLongTermMemoryText(20);
        TestRunner::AssertContains(text, "User prefers concise output");
    }

    fs::remove_all(base);
}

TEST(memory_runtime, RelationUpdateMarksOldInactive)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_relation_update_test";
    fs::remove_all(base);
    fs::create_directories(base / "memory_runtime");

    {
        MemorySqliteStore store((base / "memory_runtime" / "memory.db").string());
        TestRunner::AssertTrue(store.Initialize());

        MemoryRelation rel1;
        rel1.fromEntity = "entity:user";
        rel1.relation = "prefers";
        rel1.toEntity = "entity:verbose_style";
        rel1.confidence = 0.7F;
        rel1.sourceRefs = {"event://1"};
        TestRunner::AssertTrue(store.SaveRelation(rel1));

        MemoryRelation rel2;
        rel2.fromEntity = "entity:user";
        rel2.relation = "prefers";
        rel2.toEntity = "entity:concise_style";
        rel2.confidence = 0.9F;
        rel2.sourceRefs = {"event://2"};
        TestRunner::AssertTrue(store.SaveRelation(rel2));
    }

    {
        MemorySqliteStore store((base / "memory_runtime" / "memory.db").string());
        TestRunner::AssertTrue(store.Initialize());
        std::string text = store.LoadLongTermMemoryText(20);
        TestRunner::AssertContains(text, "entity:user prefers entity:concise_style");
        TestRunner::AssertTrue(text.find("entity:user prefers entity:verbose_style") == std::string::npos);
    }

    fs::remove_all(base);
}

TEST(memory_runtime, SchemaMigrationAddsActiveColumns)
{
    fs::path base = fs::temp_directory_path() / "jiuwen_memory_migration_test";
    fs::remove_all(base);
    fs::create_directories(base);

    {
        MemorySqliteStore store((base / "memory.db").string());
        TestRunner::AssertTrue(store.Initialize());
    }

    {
        MemorySqliteStore store((base / "memory.db").string());
        TestRunner::AssertTrue(store.Initialize());

        MemoryEntity entity;
        entity.id = "entity:migration_test";
        entity.type = "topic";
        entity.name = "Migration";
        entity.summary = "Schema migration works";
        entity.confidence = 0.8F;
        TestRunner::AssertTrue(store.SaveEntity(entity));

        MemoryRelation relation;
        relation.fromEntity = "entity:user";
        relation.relation = "mentions";
        relation.toEntity = "entity:migration_test";
        relation.confidence = 0.7F;
        TestRunner::AssertTrue(store.SaveRelation(relation));

        TestRunner::AssertTrue(store.MarkEntityObsolete("entity:migration_test", "entity:new"));
    }

    fs::remove_all(base);
}

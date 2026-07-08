// Tests for AgentConfig JSON serialization + AgentConfigStore.

#include <filesystem>
#include <fstream>

#include "include/config/agent_config_json.h"
#include "include/config/agent_config_store.h"
#include "include/types.h"
#include "test_runner.h"

#include "third_party/include/nlohmann/json.hpp"

namespace fs = std::filesystem;
using namespace jiuwen;

TEST(agent_config_json, RoundTripBasic)
{
    AgentConfig cfg;
    cfg.id = "test-agent";
    cfg.name = "Test";
    cfg.mode = AgentWorkMode::REACT;
    cfg.maxIterations = 17;
    cfg.dataBasePath = "./data";
    cfg.modelConfig.baseUrl = "http://localhost/v1";
    cfg.modelConfig.apiKey = "sk-x";
    cfg.modelConfig.modelName = "gpt-x";
    cfg.modelConfig.formatType = ModelFormatType::OPENAI;
    cfg.modelConfig.extraParams.Set("temperature", 0.3f);
    cfg.modelConfig.extraParams.Set("max_tokens", 1024);
    cfg.contextConfig.maxMessages = 99;
    cfg.memoryConfig.enabled = true;
    cfg.memoryConfig.mode = "server";
    cfg.memoryConfig.provider = "http.server";
    cfg.memoryConfig.serverUrl = "http://127.0.0.1:8090";
    cfg.memoryConfig.serverApiKey = "test-key";
    cfg.memoryConfig.serverTimeoutSeconds = 3;
    cfg.memoryConfig.enablePayloadOffload = true;
    cfg.memoryConfig.modelEnabled = true;
    cfg.memoryConfig.modelName = "gpt-4o";
    cfg.memoryConfig.modelBaseUrl = "http://localhost/v1";
    cfg.promptTemplates["sys"] = PromptResource{PromptResourceType::TEXT, "hi"};
    cfg.defaultTools = {"a", "b"};

    auto j = AgentConfigToJson(cfg);

    AgentConfig parsed;
    MergeAgentConfigFromJson(j, parsed);
    TestRunner::AssertEq(parsed.id, std::string("test-agent"));
    TestRunner::AssertEq(parsed.name, std::string("Test"));
    TestRunner::AssertEq(parsed.maxIterations, 17);
    TestRunner::AssertTrue(parsed.mode == AgentWorkMode::REACT);
    TestRunner::AssertEq(parsed.modelConfig.baseUrl, std::string("http://localhost/v1"));
    TestRunner::AssertEq(parsed.modelConfig.modelName, std::string("gpt-x"));
    TestRunner::AssertEq(parsed.contextConfig.maxMessages, 99);
    TestRunner::AssertTrue(parsed.memoryConfig.enabled);
    TestRunner::AssertEq(parsed.memoryConfig.mode, std::string("server"));
    TestRunner::AssertEq(parsed.memoryConfig.provider, std::string("http.server"));
    TestRunner::AssertEq(parsed.memoryConfig.serverUrl, std::string("http://127.0.0.1:8090"));
    TestRunner::AssertEq(parsed.memoryConfig.serverApiKey, std::string("test-key"));
    TestRunner::AssertEq(parsed.memoryConfig.serverTimeoutSeconds, 3);
    TestRunner::AssertTrue(parsed.memoryConfig.enablePayloadOffload);
    TestRunner::AssertTrue(parsed.memoryConfig.modelEnabled);
    TestRunner::AssertEq(parsed.memoryConfig.modelName, std::string("gpt-4o"));
    TestRunner::AssertEq(parsed.memoryConfig.modelBaseUrl, std::string("http://localhost/v1"));
    TestRunner::AssertEq(parsed.defaultTools.size(), (size_t)2);
    TestRunner::AssertEq(parsed.promptTemplates["sys"].value, std::string("hi"));
    const int* mt = parsed.modelConfig.extraParams.GetPtr<int>("max_tokens");
    TestRunner::AssertTrue(mt != nullptr && *mt == 1024);
}

TEST(agent_config_json, MergePreservesUnspecifiedFields)
{
    AgentConfig base;
    base.id = "demo";
    base.name = "Default";
    base.maxIterations = 50;
    base.modelConfig.baseUrl = "http://default/v1";
    base.modelConfig.apiKey = "default-key";

    nlohmann::json override = nlohmann::json::parse(R"({
        "id": "demo",
        "maxIterations": 999,
        "modelConfig": { "apiKey": "override-key" }
    })");

    AgentConfig merged = MergeAgentConfig(base, override);
    TestRunner::AssertEq(merged.id, std::string("demo"));
    TestRunner::AssertEq(merged.name, std::string("Default")); // preserved
    TestRunner::AssertEq(merged.maxIterations, 999);             // overridden
    TestRunner::AssertEq(merged.modelConfig.baseUrl, std::string("http://default/v1")); // preserved
    TestRunner::AssertEq(merged.modelConfig.apiKey, std::string("override-key"));        // overridden
}

TEST(agent_config_store, NoFileReturnsDefaults)
{
    auto path = fs::temp_directory_path() / "jiuwen_test_no_file.json";
    fs::remove(path);

    AgentConfigStore store;
    store.SetPersistPath(path.string());
    AgentConfig def;
    def.id = "demo";
    def.name = "Default";
    def.maxIterations = 50;
    store.RegisterDefault(def);

    auto eff = store.Load();
    TestRunner::AssertEq(eff.size(), (size_t)1);
    TestRunner::AssertEq(eff["demo"].name, std::string("Default"));
}

TEST(agent_config_store, FileOverridesDefaults)
{
    auto path = fs::temp_directory_path() / "jiuwen_test_overrides.json";
    fs::remove(path);
    {
        std::ofstream out(path);
        out << R"({ "version": 1, "agents": [
            { "id": "demo", "name": "FromFile", "maxIterations": 7 }
        ] })";
    }

    AgentConfigStore store;
    store.SetPersistPath(path.string());
    AgentConfig def;
    def.id = "demo";
    def.name = "Default";
    def.maxIterations = 50;
    def.modelConfig.baseUrl = "http://default/v1";
    store.RegisterDefault(def);

    auto eff = store.Load();
    TestRunner::AssertEq(eff["demo"].name, std::string("FromFile"));
    TestRunner::AssertEq(eff["demo"].maxIterations, 7);
    TestRunner::AssertEq(eff["demo"].modelConfig.baseUrl, std::string("http://default/v1"));

    fs::remove(path);
}

TEST(agent_config_store, UpsertPersistsAndSurvivesReload)
{
    auto path = fs::temp_directory_path() / "jiuwen_test_upsert.json";
    fs::remove(path);

    AgentConfigStore store;
    store.SetPersistPath(path.string());
    AgentConfig def;
    def.id = "demo";
    def.name = "Default";
    def.maxIterations = 10;
    store.RegisterDefault(def);
    store.Load();

    AgentConfig changed = def;
    changed.maxIterations = 42;
    store.Upsert(changed);

    AgentConfigStore store2;
    store2.SetPersistPath(path.string());
    store2.RegisterDefault(def);
    auto eff = store2.Load();
    TestRunner::AssertEq(eff["demo"].maxIterations, 42);

    fs::remove(path);
}

TEST(agent_config_json, IdleConsolidationSecondsMigratesFromContextConfig)
{
    // Old-style config persisted the value under contextConfig. The double
    // read fallback must pick it up into memoryConfig.idleConsolidationSeconds.
    // The field no longer exists on ContextConfig (migrated), so this test
    // only checks the memoryConfig side.
    nlohmann::json legacy = nlohmann::json::parse(R"({
        "id": "demo",
        "contextConfig": { "idleConsolidationSeconds": 77 },
        "memoryConfig": { "enabled": true }
    })");

    AgentConfig parsed;
    MergeAgentConfigFromJson(legacy, parsed);
    TestRunner::AssertEq(parsed.memoryConfig.idleConsolidationSeconds, 77);
}

TEST(agent_config_json, IdleConsolidationSecondsPrefersMemoryConfig)
{
    // New-style config: memoryConfig wins over the legacy contextConfig
    // location when both are present (so users can override the legacy
    // value by setting the new key).
    nlohmann::json both = nlohmann::json::parse(R"({
        "id": "demo",
        "contextConfig": { "idleConsolidationSeconds": 77 },
        "memoryConfig": { "idleConsolidationSeconds": 33 }
    })");

    AgentConfig parsed;
    MergeAgentConfigFromJson(both, parsed);
    TestRunner::AssertEq(parsed.memoryConfig.idleConsolidationSeconds, 33);
}

TEST(agent_config_json, IdleConsolidationSecondsSerializedOnlyInMemoryConfig)
{
    // Single-source migration: serialization must NOT write the value back
    // into contextConfig. A load+save cycle should leave contextConfig
    // without the legacy key.
    AgentConfig cfg;
    cfg.id = "demo";
    cfg.memoryConfig.idleConsolidationSeconds = 55;

    auto j = AgentConfigToJson(cfg);
    TestRunner::AssertTrue(!j["contextConfig"].contains("idleConsolidationSeconds"),
                           "contextConfig must not serialize idleConsolidationSeconds");
    TestRunner::AssertEq(j["memoryConfig"]["idleConsolidationSeconds"].get<int>(), 55);

    // Round-trip back into memoryConfig only.
    AgentConfig parsed;
    MergeAgentConfigFromJson(j, parsed);
    TestRunner::AssertEq(parsed.memoryConfig.idleConsolidationSeconds, 55);
}

TEST(agent_config_json, ExcludedConsolidationSessionIdsRoundTrip)
{
    AgentConfig cfg;
    cfg.id = "demo";
    cfg.memoryConfig.excludedConsolidationSessionIds = {"__CRON__", "__HEARTBEAT__"};

    auto j = AgentConfigToJson(cfg);
    TestRunner::AssertTrue(j["memoryConfig"].contains("excludedConsolidationSessionIds"));
    auto ids = j["memoryConfig"]["excludedConsolidationSessionIds"];
    TestRunner::AssertEq(ids.size(), (size_t)2);
    TestRunner::AssertEq(ids[0].get<std::string>(), std::string("__CRON__"));
    TestRunner::AssertEq(ids[1].get<std::string>(), std::string("__HEARTBEAT__"));

    AgentConfig parsed;
    MergeAgentConfigFromJson(j, parsed);
    TestRunner::AssertEq(parsed.memoryConfig.excludedConsolidationSessionIds.size(), (size_t)2);
    TestRunner::AssertEq(parsed.memoryConfig.excludedConsolidationSessionIds[0], std::string("__CRON__"));
    TestRunner::AssertEq(parsed.memoryConfig.excludedConsolidationSessionIds[1], std::string("__HEARTBEAT__"));
}

TEST(agent_config_json, ExcludedConsolidationSessionIdsDefaultsEmpty)
{
    // No key in JSON: stays at the default (empty vector), backward compat.
    nlohmann::json minimal = nlohmann::json::parse(R"({ "id": "demo" })");
    AgentConfig parsed;
    MergeAgentConfigFromJson(minimal, parsed);
    TestRunner::AssertTrue(parsed.memoryConfig.excludedConsolidationSessionIds.empty());
}

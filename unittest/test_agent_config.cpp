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
    cfg.memoryConfig.hotMessages = 12;
    cfg.memoryConfig.enablePayloadOffload = true;
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
    TestRunner::AssertEq(parsed.memoryConfig.hotMessages, 12);
    TestRunner::AssertTrue(parsed.memoryConfig.enablePayloadOffload);
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

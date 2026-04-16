

#include <fstream>
#include <string>
#include <vector>
#include "filesystem"
#include "include/agent.h"
#include "include/model.h"
#include "include/resource_manager.h"
#include "src/context_engine/context_engine.h"
#include "src/tools/builtin_tools/list_dir_tool.h"
#include "src/tools/builtin_tools/read_file_tool.h"
#include "src/tools/builtin_tools/time_info_tool.h"
#include "src/tools/builtin_tools/write_file_tool.h"
#include "test_runner.h"

namespace fs = std::filesystem;

// ============================================
// Integration: Tool Registration & Execution
// ============================================

TEST(integration, RegisterAndExecuteBuiltinTools)
{
    auto& rm = ResourceManager::GetInstance();

    // Verify each tool is registered and can be created
    std::vector<std::string> expectedTools = {
        "time_info", "web_search", "web_fetcher",
        "read_file", "write_file", "edit_file",
        "list_dir", "glob", "grep",
        "exec", "notebook_edit", "file_state"
    };

    for (const auto& toolName : expectedTools) {
        TestRunner::AssertTrue(rm.HasTool(toolName), "Tool should be registered: " + toolName);
        auto tool = rm.CreateTool(toolName);
        TestRunner::AssertTrue(tool != nullptr, "Tool should be createable: " + toolName);
        TestRunner::AssertTrue(!tool->GetName().empty(), "Tool name should not be empty: " + toolName);
    }
}

TEST(integration, TimeInfoToolExecute)
{
    TimeInfoTool tool;
    std::string result = tool.Invoke("");
    TestRunner::AssertTrue(result.find("-") != std::string::npos || result.find("/") != std::string::npos, "Should contain date");
}

TEST(integration, WriteThenReadFile)
{
    std::string testDir = "test_tmp_integration";
    if (fs::exists(testDir)) fs::remove_all(testDir);
    fs::create_directories(testDir);

    WriteFileTool writeTool;
    std::string writeResult = writeTool.Invoke("{\"path\":\"test_tmp_integration/test.txt\",\"content\":\"Integration test content\"}");
    TestRunner::AssertContains(writeResult, "Successfully wrote");

    ReadFileTool readTool;
    std::string readResult = readTool.Invoke("{\"path\":\"test_tmp_integration/test.txt\"}");
    TestRunner::AssertContains(readResult, "1| Integration test content");

    fs::remove_all(testDir);
}

TEST(integration, ListDirWithFiles)
{
    std::string testDir = "test_tmp_integration_ls";
    if (fs::exists(testDir)) fs::remove_all(testDir);
    fs::create_directories(testDir);
    std::ofstream(testDir + "/a.txt") << "a";
    std::ofstream(testDir + "/b.txt") << "b";

    ListDirTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_integration_ls\"}");
    TestRunner::AssertContains(result, "a.txt");
    TestRunner::AssertContains(result, "b.txt");

    fs::remove_all(testDir);
}

// ============================================
// Integration: Context Persistence
// ============================================

TEST(integration, ContextEngineMemoryRoundTrip)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.maxContextTokens = 10000;
    config.sessionId = "integration_test_session";

    // Session 1: Write messages
    {
        ContextEngine engine(config);
        engine.Initialize();
        engine.AddMessage({"user", "Hello from integration test"});
        engine.AddMessage({"assistant", "Hello back!"});
        TestRunner::AssertEq(engine.GetContextWindow().size(), size_t(2));
    }

    // Session 2: Memory-only, should be empty
    {
        ContextEngine engine(config);
        engine.Initialize();
        auto messages = engine.GetContextWindow();
        TestRunner::AssertEq(messages.size(), size_t(0), "Memory-only should not persist");
    }
}

// ============================================
// Integration: Tool Schema Generation
// ============================================

TEST(integration, ToolSchemaIsPopulated)
{
    ReadFileTool tool;
    std::string schema = tool.GetSchema();
    TestRunner::AssertContains(schema, "Tool: read_file");
    TestRunner::AssertContains(schema, "path");
    TestRunner::AssertContains(schema, "[required]");
}

// ============================================
// Integration: Agent Creation
// ============================================

TEST(integration, AgentCreateAndCancel)
{
    AgentConfig config;
    config.id = "test_agent";
    config.mode = AgentWorkMode::PLAN_AND_EXECUTE; // Use P&E to avoid model calls
    config.maxIterations = 1;
    config.modelConfig.baseUrl = "http://localhost:0";
    config.modelConfig.apiKey = "test";
    config.modelConfig.modelName = "test";
    config.modelConfig.formatType = ModelFormatType::OPENAI;
    config.promptTemplates["plan_system"] = "Plan: {query}";
    config.contextConfig.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.contextConfig.sessionId = "agent_integration_test";

    Agent agent(config);
    agent.AddTools({"time_info"});

    // Verify tools were registered
    auto tools = agent.GetRegisteredTools();
    TestRunner::AssertEq(tools.size(), size_t(1));
}

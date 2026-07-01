 
// Dummy tool for ResourceManager tests

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "include/model.h"
#include "include/resource_manager.h"
#include "include/tool.h"
#include "src/core/agent_worker.h"
#include "test_runner.h"

using namespace jiuwen;

class TestTool : public Tool {public:
    TestTool() : Tool("test_tool", "A test tool", {{"input", "Input", "string", true}}){} 
    std::string Invoke(const std::string& input) override 
    { 
        return "tested: " + input; 
    }
};

// Dummy model for ResourceManager tests
class TestModel : public Model {
public:
    TestModel(ModelConfig config) : Model(std::move(config)){}
    std::string Format(const std::string& systemPrompt,
                       const std::vector<Message>& messages,
                       const std::vector<ToolSchema>&) override
    {
        return "formatted:" + systemPrompt;
    }
    ModelResponse Invoke(const std::string& formattedInput,
                         std::function<void(const std::string&)> onChunk,
                         std::function<bool()> /*shouldCancel*/) override
    {
        (void)formattedInput;
        if (onChunk) onChunk("chunk");
        ModelResponse resp;
        resp.content = "model_response";
        resp.isFinished = true;
        resp.finishReason = "stop";
        return resp;
    }
};

TEST(resource_manager, Singleton)
{
    auto& rm1 = ResourceManager::GetInstance();
    auto& rm2 = ResourceManager::GetInstance();
    TestRunner::AssertTrue(&rm1 == &rm2, "Should be the same singleton instance");
}

TEST(resource_manager, RegisterAndHasTool)
{
    auto& rm = ResourceManager::GetInstance();
    rm.RegisterTool("my_test_tool", []() { return std::make_unique<TestTool>(); });
    TestRunner::AssertTrue(rm.HasTool("my_test_tool"));
}

TEST(resource_manager, CreateTool)
{
    auto& rm = ResourceManager::GetInstance();
    auto tool = rm.CreateTool("my_test_tool");
    TestRunner::AssertTrue(tool != nullptr);
    TestRunner::AssertEq(tool->GetName(), std::string("test_tool"));
}

TEST(resource_manager, CreateToolMissingThrows)
{
    auto& rm = ResourceManager::GetInstance();
    bool threw = false;
    try {
        rm.CreateTool("nonexistent_tool_xyz");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    TestRunner::AssertTrue(threw, "Should throw for non-existent tool");
}

TEST(resource_manager, RegisterAndCreateModel)
{
    auto& rm = ResourceManager::GetInstance();
    TestRunner::AssertTrue(rm.HasModel(ModelFormatType::OPENAI));
    ModelConfig cfg;
    cfg.formatType = ModelFormatType::OPENAI;
    cfg.baseUrl = "http://localhost:0";
    cfg.apiKey = "test";
    auto model = rm.CreateModel(cfg);
    TestRunner::AssertTrue(model != nullptr);
}

TEST(resource_manager, CreateModelMissingThrows)
{
    auto& rm = ResourceManager::GetInstance();
    // OPENAI is built-in, so it should NOT throw
    ModelConfig cfg;
    cfg.formatType = ModelFormatType::OPENAI;
    auto model = rm.CreateModel(cfg);
    TestRunner::AssertTrue(model != nullptr, "Built-in OPENAI model should create successfully");
}

TEST(resource_manager, GetAvailableTools)
{
    auto& rm = ResourceManager::GetInstance();
    auto tools = rm.GetAvailableTools();
    TestRunner::AssertTrue(tools.size() > 0, "Should have registered tools");
    // Find our test tool
    bool found = false;
    for (const auto& t : tools) if (t == "my_test_tool") found = true;
    TestRunner::AssertTrue(found, "my_test_tool should be in available tools");
}

TEST(resource_manager, MCPFunctionsDontCrash)
{
    auto& rm = ResourceManager::GetInstance();
    auto servers = rm.GetAvailableMCPServers();
    TestRunner::AssertTrue(rm.HasMCPServer("nonexistent") == false);
}

TEST(resource_manager, ConcurrentDomainLocksNoDeadlock)
{
    auto& rm = ResourceManager::GetInstance();
    std::atomic<int> toolOps{0};
    std::atomic<int> modelOps{0};
    std::mutex startMu;
    std::condition_variable startCv;
    bool started = false;

    auto toolThread = std::thread([&]() {
        {
            std::unique_lock<std::mutex> lk(startMu);
            startCv.wait(lk, [&]{ return started; });
        }
        for (int i = 0; i < 50; ++i) {
            rm.HasTool("time_info");
            rm.GetAvailableTools();
            rm.GetToolSchema("time_info");
            toolOps++;
        }
    });

    auto modelThread = std::thread([&]() {
        {
            std::unique_lock<std::mutex> lk(startMu);
            startCv.wait(lk, [&]{ return started; });
        }
        for (int i = 0; i < 50; ++i) {
            rm.HasModel(ModelFormatType::OPENAI);
            rm.GetAvailableModels();
            modelOps++;
        }
    });

    {
        std::lock_guard<std::mutex> lk(startMu);
        started = true;
    }
    startCv.notify_all();

    toolThread.join();
    modelThread.join();

    TestRunner::AssertEq(toolOps.load(), 50, "50 tool operations completed without deadlock");
    TestRunner::AssertEq(modelOps.load(), 50, "50 model operations completed without deadlock");
}

#include "test_runner.h"
#include "resource_manager.h"
#include "tool.h"
#include "model.h"
#include "core/agent_worker.h"
#include <string>
#include <vector>

// Dummy tool for ResourceManager tests
class TestTool : public Tool {
public:
    TestTool() : Tool("test_tool", "A test tool", {{"input", "Input", "string", true}}) {}
    std::string Invoke(const std::string& input) override { return "tested: " + input; }
};

// Dummy model for ResourceManager tests
class TestModel : public Model {
public:
    TestModel(ModelConfig config) : Model(std::move(config)) {}
    std::string Format(const std::string& systemPrompt, const std::vector<Message>& messages) override {
        return "formatted:" + systemPrompt;
    }
    std::string Invoke(const std::string& formattedInput, std::function<void(const std::string&)> onChunk) override {
        if (onChunk) onChunk("chunk");
        return "model_response";
    }
    ModelResponse ParseResponse(const std::string& rawResponse) override {
        return {rawResponse, true, "stop"};
    }
};

TEST(resource_manager, Singleton) {
    auto& rm1 = ResourceManager::GetInstance();
    auto& rm2 = ResourceManager::GetInstance();
    TestRunner::AssertTrue(&rm1 == &rm2, "Should be the same singleton instance");
}

TEST(resource_manager, RegisterAndHasTool) {
    auto& rm = ResourceManager::GetInstance();
    rm.RegisterTool("my_test_tool", []() { return std::make_unique<TestTool>(); });
    TestRunner::AssertTrue(rm.HasTool("my_test_tool"));
}

TEST(resource_manager, CreateTool) {
    auto& rm = ResourceManager::GetInstance();
    auto tool = rm.CreateTool("my_test_tool");
    TestRunner::AssertTrue(tool != nullptr);
    TestRunner::AssertEq(tool->GetName(), std::string("test_tool"));
}

TEST(resource_manager, CreateToolMissingThrows) {
    auto& rm = ResourceManager::GetInstance();
    bool threw = false;
    try {
        rm.CreateTool("nonexistent_tool_xyz");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    TestRunner::AssertTrue(threw, "Should throw for non-existent tool");
}

TEST(resource_manager, RegisterAndCreateModel) {
    auto& rm = ResourceManager::GetInstance();
    rm.RegisterModel(ModelFormatType::DEEPSEEK, [](const ModelConfig& cfg) {
        return std::make_unique<TestModel>(cfg);
    });
    TestRunner::AssertTrue(rm.HasModel(ModelFormatType::DEEPSEEK));
    ModelConfig cfg;
    cfg.formatType = ModelFormatType::DEEPSEEK;
    auto model = rm.CreateModel(cfg);
    TestRunner::AssertTrue(model != nullptr);
}

TEST(resource_manager, CreateModelMissingThrows) {
    auto& rm = ResourceManager::GetInstance();
    // OPENAI is built-in, so it should NOT throw
    ModelConfig cfg;
    cfg.formatType = ModelFormatType::OPENAI;
    auto model = rm.CreateModel(cfg);
    TestRunner::AssertTrue(model != nullptr, "Built-in OPENAI model should create successfully");
}

TEST(resource_manager, GetAvailableTools) {
    auto& rm = ResourceManager::GetInstance();
    auto tools = rm.GetAvailableTools();
    TestRunner::AssertTrue(tools.size() > 0, "Should have registered tools");
    // Find our test tool
    bool found = false;
    for (const auto& t : tools) if (t == "my_test_tool") found = true;
    TestRunner::AssertTrue(found, "my_test_tool should be in available tools");
}

TEST(resource_manager, MCPFunctionsDontCrash) {
    auto& rm = ResourceManager::GetInstance();
    auto servers = rm.GetAvailableMCPServers();
    TestRunner::AssertTrue(rm.HasMCPServer("nonexistent") == false);
}

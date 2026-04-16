

#include <string>
#include "include/types.h"
#include "src/core/agent_worker.h"
#include "test_runner.h"

TEST(agent_worker_factory, CreatesReactWorker)
{
    AgentConfig config;
    config.mode = AgentWorkMode::REACT;
    auto worker = CreateAgentWorker(config);
    TestRunner::AssertTrue(worker != nullptr);
}

TEST(agent_worker_factory, CreatesPlanExecuteWorker)
{
    AgentConfig config;
    config.mode = AgentWorkMode::PLAN_AND_EXECUTE;
    auto worker = CreateAgentWorker(config);
    TestRunner::AssertTrue(worker != nullptr);
}

TEST(agent_worker_factory, CreatesWorkflowWorker)
{
    AgentConfig config;
    config.mode = AgentWorkMode::WORKFLOW;
    auto worker = CreateAgentWorker(config);
    TestRunner::AssertTrue(worker != nullptr);
}

TEST(agent_worker_factory, UnknownModeThrows)
{
    AgentConfig config;
    config.mode = static_cast<AgentWorkMode>(999);
    config.maxIterations = 1;
    config.modelConfig.formatType = ModelFormatType::OPENAI;
    bool threw = false;
    try {
        CreateAgentWorker(config);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    TestRunner::AssertTrue(threw, "Should throw invalid_argument for unknown mode");
}

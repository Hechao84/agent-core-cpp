#include "core/agent_worker.h"
#include "react_worker.h"
#include "plan_execute_worker.h"
#include "workflow_worker.h"
#include <stdexcept>

std::unique_ptr<AgentWorker> CreateAgentWorker(AgentConfig config) {
    switch (config.mode) {
        case AgentWorkMode::REACT: return std::make_unique<ReactAgentWorker>(std::move(config));
        case AgentWorkMode::PLAN_AND_EXECUTE: return std::make_unique<PlanAndExecuteAgentWorker>(std::move(config));
        case AgentWorkMode::WORKFLOW: return std::make_unique<WorkflowAgentWorker>(std::move(config));
        default: throw std::invalid_argument("Unknown work mode");
    }
}

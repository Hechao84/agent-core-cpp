#include <memory>
#include <stdexcept>
#include "src/core/agent_worker.h"
#include "src/workers/react_worker.h"

namespace jiuwen {

std::unique_ptr<AgentWorker> CreateAgentWorker(AgentConfig config)
{
    switch (config.mode) {
        case AgentWorkMode::REACT:
            return std::make_unique<ReactAgentWorker>(std::move(config));
        case AgentWorkMode::PLAN_AND_EXECUTE:
        case AgentWorkMode::WORKFLOW:
            throw std::invalid_argument(
                "AgentWorkMode PLAN_AND_EXECUTE / WORKFLOW are not implemented in this build");
        default:
            throw std::invalid_argument("Unknown work mode");
    }
}

} // namespace jiuwen

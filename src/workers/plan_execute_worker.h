#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "include/types.h"
#include "src/core/agent_worker.h"

namespace jiuwen {

class PlanAndExecuteAgentWorker : public AgentWorker {
public:
    PlanAndExecuteAgentWorker(AgentConfig config);
    std::string Invoke(const std::string& query, ContextEngine* contextEngine, std::function<void(const std::string&)> callback) override;
private:
    std::vector<std::string> GeneratePlan(const std::string& query, ContextEngine* contextEngine, std::function<void(const std::string&)> callback, uint64_t myGeneration);
    std::string ExecuteStep(const std::string& step, const std::string& context, ContextEngine* contextEngine, std::function<void(const std::string&)> callback, uint64_t myGeneration);
    std::string SynthesizeResult(const std::string& query, const std::string& context, ContextEngine* contextEngine, std::function<void(const std::string&)> callback, uint64_t myGeneration);
};

} // namespace jiuwen

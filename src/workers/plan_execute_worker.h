#pragma once
#include "types.h"
#include "core/agent_worker.h"
#include <string>
#include <functional>
#include <memory>
#include <vector>

class PlanAndExecuteAgentWorker : public AgentWorker {
public:
    PlanAndExecuteAgentWorker(AgentConfig config);
    void Invoke(const std::string& query, std::function<void(const std::string&)> callback) override;
private:
    std::vector<std::string> GeneratePlan(const std::string& query, std::function<void(const std::string&)> callback);
    std::string ExecuteStep(const std::string& step, const std::string& context, std::function<void(const std::string&)> callback);
    std::string SynthesizeResult(const std::string& query, const std::string& context, std::function<void(const std::string&)> callback);
};

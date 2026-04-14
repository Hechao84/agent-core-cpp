#pragma once
#include "types.h"
#include "core/agent_worker.h"
#include <string>
#include <functional>
#include <memory>
#include <vector>

class ReactAgentWorker : public AgentWorker {
public:
    ReactAgentWorker(AgentConfig config);
    void Invoke(const std::string& query, std::function<void(const std::string&)> callback) override;
private:
    void ReactLoop(const std::string& query, std::function<void(const std::string&)> callback);
};

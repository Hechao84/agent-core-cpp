#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "include/types.h"
#include "src/core/agent_worker.h"

class ReactAgentWorker : public AgentWorker {
public:
    ReactAgentWorker(AgentConfig config);
    void Invoke(const std::string& query, std::function<void(const std::string&)> callback) override;
private:
    void ReactLoop(const std::string& query, std::function<void(const std::string&)> callback);
};

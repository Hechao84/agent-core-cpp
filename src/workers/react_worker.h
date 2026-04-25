#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "include/types.h"
#include "src/core/agent_worker.h"

namespace jiuwen {

class ReactAgentWorker : public AgentWorker {
public:
    ReactAgentWorker(AgentConfig config);
    std::string Invoke(const std::string& query, std::function<void(const std::string&)> callback) override;
private:
    std::string ReactLoop(const std::string& query, std::function<void(const std::string&)> callback);
};

} // namespace jiuwen

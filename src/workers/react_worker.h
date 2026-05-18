#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "include/types.h"
#include "src/core/agent_worker.h"

namespace jiuwen {

class ContextEngine; // Forward declaration

class ReactAgentWorker : public AgentWorker {
public:
    ReactAgentWorker(AgentConfig config);
    std::string Invoke(const std::string& query, ContextEngine* contextEngine, std::function<void(const std::string&)> callback) override;
private:
    std::string ReactLoop(const std::string& query, ContextEngine* contextEngine, std::function<void(const std::string&)> callback, uint64_t myGeneration);
};

} // namespace jiuwen

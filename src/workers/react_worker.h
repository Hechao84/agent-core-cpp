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
        std::string ParseThought(const std::string& response);
        std::string ParseAction(const std::string& response);
        std::string ParseActionInput(const std::string& response);
        void ReactLoop(const std::string& query, std::function<void(const std::string&)> callback);
};

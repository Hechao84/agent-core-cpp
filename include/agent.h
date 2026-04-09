#pragma once
#include <string>
#include <functional>
#include <memory>
#include <vector>
#include "types.h"

class AgentWorker;

class Agent {
    public:
        Agent(AgentConfig config);
        ~Agent();
        void Invoke(const std::string& query, std::function<void(const std::string&)> callback);
        void Cancel();
        void AddTools(const std::vector<std::string>& toolNames);
    private:
        AgentConfig m_config;
        std::unique_ptr<AgentWorker> m_worker;
};

#pragma once
#include <string>
#include <functional>
#include <memory>
#include <vector>
#include "types.h"
#include "agent_export.h"

class AgentWorker;
class ContextEngine; // Forward declaration to keep header clean

class AGENT_API Agent {
    public:
        Agent(AgentConfig config);
        ~Agent();
        void Invoke(const std::string& query, std::function<void(const std::string&)> callback);
        void Cancel();
        void AddTools(const std::vector<std::string>& toolNames);
        std::vector<std::string> GetRegisteredTools() const;
    private:
        AgentConfig m_config;
        std::unique_ptr<AgentWorker> m_worker;
        std::shared_ptr<ContextEngine> m_contextEngine; // Store context engine
        std::vector<std::string> m_toolNames; // Agent owns the Master Tool List
};

#include "agent.h"
#include "agent_worker.h"
#include "resource_manager.h"
#include <iostream>

Agent::Agent(AgentConfig config) : m_config(std::move(config)) {
    m_worker = CreateAgentWorker(this->m_config);
}

Agent::~Agent() = default;

void Agent::Invoke(const std::string& query, std::function<void(const std::string&)> callback) {
    if (!m_worker) {
        callback("Error: Agent worker not initialized");
        return;
    }
    m_worker->Invoke(query, std::move(callback));
}

void Agent::Cancel() {
    if (m_worker) {
        m_worker->Cancel();
    }
}

void Agent::AddTools(const std::vector<std::string>& toolNames) {
    if (m_worker) {
        m_worker->AddTools(toolNames);
    }
}

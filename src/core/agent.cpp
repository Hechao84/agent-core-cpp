#include "agent.h"
#include "agent_worker.h"
#include "resource_manager.h"
#include "context_engine/context_engine.h"
#include "skills/skill_engine.h"
#include <iostream>
#include <algorithm>

Agent::Agent(AgentConfig config) : m_config(std::move(config)) {
    // 1. Initialize Context Engine based on config
    m_contextEngine = std::make_shared<ContextEngine>(m_config.contextConfig);
    m_contextEngine->Initialize();

    // 2. Initialize Skill Engine if directory is configured
    if (!m_config.skillDirectory.empty()) {
        m_skillEngine = std::make_shared<SkillEngine>(m_config.skillDirectory);
        m_skillEngine->Load(true); // Load from disk
    }

    // 3. Create Worker and inject engines
    m_worker = CreateAgentWorker(this->m_config);
    if (m_worker) {
        m_worker->SetContextEngine(m_contextEngine);
        if (m_skillEngine) {
            m_worker->SetSkillEngine(m_skillEngine);
        }
    }
}

Agent::~Agent() = default;

void Agent::Invoke(const std::string& query, std::function<void(const std::string&)> callback) {
    if (!m_worker || !m_contextEngine) {
        callback("Error: Agent not initialized");
        return;
    }

    // 1. Save User Query to Context BEFORE invoking
    m_contextEngine->AddMessage({"user", query});

    // 2. Intercept response to save it to Context
    std::string accumulatedResponse;
    auto wrappedCallback = [this, &accumulatedResponse, callback](const std::string& chunk) {
        accumulatedResponse += chunk;
        callback(chunk);
    };

    m_worker->Invoke(query, wrappedCallback);

    // 3. Save Assistant Response to Context AFTER invoking
    // We only save if we got something meaningful (not just error messages if possible, 
    // but for simplicity we save whatever the model returned)
    m_contextEngine->AddMessage({"assistant", accumulatedResponse});
}

void Agent::Cancel() {
    if (m_worker) {
        m_worker->Cancel();
    }
}

void Agent::AddTools(const std::vector<std::string>& toolNames) {
    // 1. Agent updates its Master List (Source of Truth)
    for (const auto& name : toolNames) {
        bool exists = std::find(m_toolNames.begin(), m_toolNames.end(), name) != m_toolNames.end();
        if (!exists) {
            m_toolNames.push_back(name);
        }
    }
    
    // 2. Agent forwards tools to Worker (Consumer)
    if (m_worker) {
        m_worker->AddTools(toolNames);
    }
}

std::vector<std::string> Agent::GetRegisteredTools() const {
    return m_toolNames;
}

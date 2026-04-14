#include "agent.h"
#include "agent_worker.h"
#include "resource_manager.h"
#include "context_engine/context_engine.h"
#include "skills/skill_engine.h"
#include <iostream>
#include <algorithm>

Agent::Agent(AgentConfig config) : config_(std::move(config))
{
    // 1. Initialize Context Engine based on config
    contextEngine_ = std::make_shared<ContextEngine>(config_.contextConfig);
    contextEngine_->Initialize();

    // 2. Initialize Skill Engine if directory is configured
    if (!config_.skillDirectory.empty()) {
        skillEngine_ = std::make_shared<SkillEngine>(config_.skillDirectory);
        skillEngine_->Load(true); // Load from disk
    }

    // 3. Create Worker and inject engines
    worker_ = CreateAgentWorker(this->config_);
    if (worker_) {
        worker_->SetContextEngine(contextEngine_);
        if (skillEngine_) {
            worker_->SetSkillEngine(skillEngine_);
        }
    }
}

Agent::~Agent() = default;

void Agent::Invoke(const std::string& query, std::function<void(const std::string&)> callback)
{
    if (!worker_ || !contextEngine_) {
        callback("Error: Agent not initialized");
        return;
    }

    // 1. Save User Query to Context BEFORE invoking
    contextEngine_->AddMessage({"user", query});

    // 2. Intercept response to save it to Context
    std::string accumulatedResponse;
    auto wrappedCallback = [this, &accumulatedResponse, callback](const std::string& chunk)
    {
        accumulatedResponse += chunk;
        callback(chunk);
    };

    worker_->Invoke(query, wrappedCallback);

    // 3. Save Assistant Response to Context AFTER invoking
    // We only save if we got something meaningful (not just error messages if possible,
    // but for simplicity we save whatever the model returned)
    contextEngine_->AddMessage({"assistant", accumulatedResponse});
}

void Agent::Cancel()
{
    if (worker_) {
        worker_->Cancel();
    }
}

void Agent::AddTools(const std::vector<std::string>& toolNames)
{
    // 1. Agent updates its Master List (Source of Truth)
    for (const auto& name : toolNames) {
        bool exists = std::find(toolNames_.begin(), toolNames_.end(), name) != toolNames_.end();
        if (!exists) {
            toolNames_.push_back(name);
        }
    }

    // 2. Agent forwards tools to Worker (Consumer)
    if (worker_) {
        worker_->AddTools(toolNames);
    }
}

std::vector<std::string> Agent::GetRegisteredTools() const
{
    return toolNames_;
}

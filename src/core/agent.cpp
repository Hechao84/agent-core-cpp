

#include "include/agent.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include "include/resource_manager.h"
#include "src/context_engine/context_engine.h"
#include "src/core/agent_worker.h"
#include "src/skills/skill_engine.h"
#include "src/tools/builtin_tools/skill_search_tool.h"
#include "src/utils/data_dir.h"

namespace fs = std::filesystem;


Agent::Agent(AgentConfig config) : config_(std::move(config))
{
    // 0. Initialize global DataDir from context storage path
    // Extract base data directory: "./data/context" -> "./data"
    std::string dataBasePath = "./data";
    if (!config_.contextConfig.storagePath.empty()) {
        dataBasePath = fs::path(config_.contextConfig.storagePath).parent_path().string();
    }
    InitDataDir(dataBasePath);
    contextEngine_ = std::make_shared<ContextEngine>(config_.contextConfig);
    contextEngine_->Initialize();

    // 2. Initialize Skill Engine if directory is configured
    if (!config_.skillDirectory.empty()) {
        skillEngine_ = std::make_shared<SkillEngine>(config_.skillDirectory);
        skillEngine_->Load(true); // Load from disk
        // Inject SkillEngine reference into SkillSearchTool
        SkillSearchTool::SetEngine(skillEngine_.get());
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

std::string Agent::Invoke(const std::string& query, std::function<void(const std::string&)> callback)
{
    if (!worker_ || !contextEngine_) {
        callback("Error: Agent not initialized");
        return "Error: Agent not initialized";
    }

    // 1. Save User Query to Context BEFORE invoking
    contextEngine_->AddMessage({"user", query});

    // 2. Call worker and get the final answer directly
    std::string finalAnswer = worker_->Invoke(query, callback);

    // 3. Save Assistant Response to Context AFTER invoking
    contextEngine_->AddMessage({"assistant", finalAnswer});

    return finalAnswer;
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

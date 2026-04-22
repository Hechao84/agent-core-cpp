#include "include/agent.h"
#include <algorithm>
#include <chrono>
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

    // 1. Initialize Skill Engine if directory is configured
    if (!config_.skillDirectory.empty()) {
        skillEngine_ = std::make_shared<SkillEngine>(config_.skillDirectory);
        skillEngine_->Load(true); // Load from disk
        // Inject SkillEngine reference into SkillSearchTool
        SkillSearchTool::SetEngine(skillEngine_.get());
    }

    // 2. Create Worker and inject engines
    worker_ = CreateAgentWorker(this->config_);
    if (worker_) {
        worker_->SetContextEngine(contextEngine_);
        if (skillEngine_) {
            worker_->SetSkillEngine(skillEngine_);
        }
    }

    // 3. Start background consolidation thread (sleep-time memory)
    consolidationThread_ = std::thread(&Agent::ConsolidationLoop, this);
}

Agent::~Agent()
{
    running_ = false;
    cv_.notify_all();
    if (consolidationThread_.joinable()) {
        consolidationThread_.join();
    }
}

std::string Agent::Invoke(const std::string& query, std::function<void(const std::string&)> callback)
{
    if (!worker_ || !contextEngine_) {
        callback("Error: Agent not initialized");
        return "Error: Agent not initialized";
    }

    // 1. Mark as active to pause consolidation
    {
        std::lock_guard<std::mutex> lock(mutex_);
        isActive_ = true;
    }

    // 2. Save User Query to Context BEFORE invoking
    contextEngine_->AddMessage({"user", query});

    // 3. Call worker and get the final answer directly
    std::string finalAnswer = worker_->Invoke(query, callback);

    // 4. Save Assistant Response to Context AFTER invoking
    contextEngine_->AddMessage({"assistant", finalAnswer});

    // 5. Mark as inactive and notify consolidation thread
    {
        std::lock_guard<std::mutex> lock(mutex_);
        isActive_ = false;
    }
    cv_.notify_all();

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

std::string Agent::GetMemoryContent() const
{
    if (contextEngine_) {
        return contextEngine_->GetMemoryContent();
    }
    return "";
}

void Agent::UpdateMemory(const std::string& keyFacts)
{
    if (contextEngine_) {
        contextEngine_->UpdateMemory(keyFacts);
    }
}

void Agent::ClearMemory()
{
    if (contextEngine_) {
        contextEngine_->ClearMemory();
    }
}

void Agent::ConsolidationLoop()
{
    while (running_) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto idleSeconds = static_cast<unsigned int>(config_.contextConfig.idleConsolidationSeconds);
        if (idleSeconds <= 0) idleSeconds = 60;

        // Wait until idle timeout or notified of new activity
        cv_.wait_for(lock, std::chrono::seconds(idleSeconds), [this]() {
            return !isActive_ || !running_;
        });

        // If agent became active again, reset and keep waiting
        if (isActive_ || !running_) {
            continue;
        }

        // Agent is idle, trigger consolidation
        lock.unlock();
        ConsolidateMemory();
    }
}

void Agent::ConsolidateMemory()
{
    if (!contextEngine_) return;

    std::string conversationText = contextEngine_->GetConsolidationPayload(100);
    if (conversationText.empty()) return;

    std::string existingMemory = contextEngine_->GetMemoryContent();

    std::string summaryPrompt =
        "You are a memory management assistant. You have access to the current long-term memory and a recent conversation.\n"
        "Your task is to produce an updated, comprehensive memory by applying the following rules:\n"
        "1. PRESERVE: Keep existing facts that are still valid and relevant.\n"
        "2. ADD: Add important new facts, preferences, decisions, or context from the conversation.\n"
        "3. UPDATE: If a new fact conflicts with or supersedes an existing one, replace the old one.\n"
        "4. REMOVE: Discard facts that are no longer relevant or have been invalidated.\n\n"
        "Rules for extraction:\n"
        "- Only retain truly important information (user preferences, context, decisions, key facts).\n"
        "- Be concise and specific. Use bullet points.\n"
        "- Do NOT include trivial details or step-by-step tool usage.\n\n"
        "Current Memory:\n" + (existingMemory.empty() ? "(Empty)" : existingMemory) + "\n\n"
        "Recent Conversation:\n" + conversationText + "\n\n"
        "Output only the fully updated memory content below. Do not include explanations:";

    try {
        auto model = ResourceManager::GetInstance().CreateModel(config_.modelConfig);
        std::string formatted = model->Format(summaryPrompt, {});
        std::string updatedMemory = model->Invoke(formatted, nullptr);

        if (!updatedMemory.empty() && updatedMemory.find("no information") == std::string::npos) {
            contextEngine_->OverwriteMemory(updatedMemory);
        }
    } catch (const std::exception& e) {
        std::cerr << "Memory consolidation failed: " << e.what() << std::endl;
    }
}

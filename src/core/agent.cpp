#include "include/agent.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "include/model.h"
#include "include/resource_manager.h"
#include "src/context_engine/context_engine.h"
#include "src/core/agent_worker.h"
#include "src/core/dream_processor.h"
#include "src/core/history_store.h"
#include "src/skills/skill_engine.h"
#include "src/tools/builtin_tools/skill_search_tool.h"
#include "src/utils/data_dir.h"
#include "src/utils/logger.h"

namespace fs = std::filesystem;

namespace jiuwen {

Agent::Agent(AgentConfig config) : config_(std::move(config))
{
    std::string dataPath = config_.dataBasePath.empty() ? "./data" : config_.dataBasePath;
    fs::create_directories(fs::path(dataPath) / "memory");
    fs::create_directories(fs::path(dataPath) / "sessions");
    InitDataDir(dataPath);

    if (!config_.skillDirectory.empty()) {
        skillEngine_ = std::make_shared<SkillEngine>(config_.skillDirectory);
        skillEngine_->Load(true);
        SkillSearchTool::SetEngine(skillEngine_.get());
    }

    worker_ = CreateAgentWorker(this->config_);
    if (skillEngine_) {
        worker_->SetSkillEngine(skillEngine_);
    }

    historyStore_ = std::make_unique<HistoryStore>(dataPath);

    config_.dreamConfig.dataBasePath = dataPath;
    dreamProcessor_ = std::make_unique<DreamProcessor>(config_.dreamConfig);

    consolidationThread_ = std::thread(&Agent::ConsolidationLoop, this);

    LOG(INFO) << "[Agent] Single-Agent initialized with Dream memory consolidation, maxConcurrentSessions="
              << config_.maxConcurrentSessions;
}

Agent::~Agent()
{
    running_ = false;
    if (worker_) {
        worker_->Cancel();
    }
    {
        std::lock_guard<std::mutex> lock(consolidationMutex_);
        cv_.notify_all();
    }
    if (consolidationThread_.joinable()) {
        consolidationThread_.join();
    }
    LOG(INFO) << "[Agent] Shutdown complete";
}

void Agent::SetContextEngineGetter(
    std::function<std::shared_ptr<ContextEngine>(const std::string&)> getter)
{
    contextEngineGetter_ = getter;
}

std::string Agent::Invoke(const std::string& sessionId, const std::string& query,
                          std::function<void(const std::string&)> callback)
{
    if (!worker_) {
        callback("[STATUS] Error: Agent not initialized");
        return "Error: Agent not initialized";
    }

    auto contextEngine = contextEngineGetter_(sessionId);
    if (!contextEngine) {
        callback("[STATUS] Error: ContextEngine not found for session=" + sessionId);
        return "Error: Session context not found";
    }

    // 1. Mark session as active
    NotifySessionActive(sessionId);

    // 2. Save User Query to Context BEFORE invoking
    contextEngine->AddMessage({"user", query});

    // 2b. Record to Dream history store
    if (historyStore_) {
        historyStore_->AppendEntry("user", query);
    }

    // 3. Call worker and get the final answer (pass contextEngine directly to avoid race conditions)
    // We wrap the callback to intercept and save tool calls/responses to context
    std::string finalAnswer = worker_->Invoke(query, contextEngine.get(), [callback, contextEngine, sessionId](const std::string& response) {
        if (!response.empty() && callback) {
            callback(response);
        }

        // Intercept tool calls and responses to save them to context using standard roles
        if (response.find("[TOOL_CALLS]") != std::string::npos) {
            std::string content = response;
            size_t start = content.find("[TOOL_CALLS]");
            if (start != std::string::npos) {
                size_t end = content.find("[/TOOL_CALLS]");
                // If end tag is missing, take the whole thing
                std::string payload = (end != std::string::npos) ? content.substr(start + 12, end - start - 12) : content.substr(start + 12);
                
                // Try to find the first '{' to extract pure JSON if tags are messy
                size_t jsonStart = payload.find('{');
                if (jsonStart != std::string::npos) {
                    payload = payload.substr(jsonStart);
                }
                
                // Save with "assistant" role containing the tool call JSON
                contextEngine->AddMessage({"assistant", payload});
            }
        } else if (response.find("[TOOL_RESPONSE]") != std::string::npos) {
            std::string content = response;
            size_t start = content.find("[TOOL_RESPONSE]");
            if (start != std::string::npos) {
                size_t end = content.find("[/TOOL_RESPONSE]");
                std::string payload = (end != std::string::npos) ? content.substr(start + 15, end - start - 15) : content.substr(start + 15);
                
                // Save with "tool" role
                contextEngine->AddMessage({"tool", payload});
            }
        }
    });

    // 5. Save Assistant Response to Context AFTER invoking
    if (!finalAnswer.empty()) {
        contextEngine->AddMessage({"assistant", finalAnswer});

        // 5b. Record to Dream history store
        if (historyStore_) {
            historyStore_->AppendEntry("assistant", finalAnswer);
        }
    }

    // 6. Mark session as idle, notify consolidation
    NotifySessionIdle(sessionId);

    return finalAnswer;
}

void Agent::NotifySessionActive(const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(sessionActivityMutex_);
    auto& entry = sessionActivity_[sessionId];
    entry.sessionId = sessionId;
    entry.isBusy = true;
}

void Agent::NotifySessionIdle(const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(sessionActivityMutex_);
    auto it = sessionActivity_.find(sessionId);
    if (it != sessionActivity_.end()) {
        it->second.isBusy = false;
    }
    cv_.notify_all();
}

bool Agent::IsSessionBusy(const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(sessionActivityMutex_);
    auto it = sessionActivity_.find(sessionId);
    if (it == sessionActivity_.end()) return false;
    return it->second.isBusy;
}

void Agent::Cancel()
{
    if (worker_) {
        worker_->Cancel();
    }
}

void Agent::AddTools(const std::vector<std::string>& toolNames)
{
    for (const auto& name : toolNames) {
        bool exists = std::find(toolNames_.begin(), toolNames_.end(), name) != toolNames_.end();
        if (!exists) {
            toolNames_.push_back(name);
        }
    }

    if (worker_) {
        worker_->AddTools(toolNames);
    }
}

std::vector<std::string> Agent::GetRegisteredTools() const
{
    return toolNames_;
}

void Agent::ConsolidationLoop()
{
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(consolidationMutex_);
            auto idleSeconds = static_cast<unsigned int>(config_.contextConfig.idleConsolidationSeconds);
            if (idleSeconds <= 0) idleSeconds = 60;

            cv_.wait_for(lock, std::chrono::seconds(idleSeconds), [this]() {
                return !running_;
            });
            if (!running_) break;
        }

        // Check if any session is idle (not busy) before running Dream
        bool anyIdle = false;
        {
            std::lock_guard<std::mutex> lock(sessionActivityMutex_);
            for (const auto& p : sessionActivity_) {
                if (!p.second.isBusy) {
                    anyIdle = true;
                    break;
                }
            }
        }

        if (!anyIdle) continue;

        try {
            auto model = ResourceManager::GetInstance().CreateModel(config_.modelConfig);
            bool didWork = dreamProcessor_->Run(model.get(), historyStore_.get());

            if (didWork) {
                LOG(INFO) << "[Dream] Memory consolidation completed";
            }
        } catch (const std::exception& e) {
            LOG(WARN) << "[Dream] Consolidation failed: " << e.what();
        }
    }
}

} // namespace jiuwen

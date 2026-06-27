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
#include <unordered_set>
#include <vector>

#include "include/model.h"
#include "include/resource_manager.h"
#include "src/context_engine/context_engine.h"
#include "src/core/agent_worker.h"
#include "src/core/history_store.h"
#include "src/memory/long_term_consolidator.h"
#include "src/core/worker_env.h"
#include "src/skills/skill_engine.h"
#include "src/tools/builtin_tools/skill_search_tool.h"
#include "src/utils/data_dir.h"
#include "src/utils/logger.h"

namespace fs = std::filesystem;

namespace jiuwen {

namespace {

// Wraps a framework Model as a MemoryModelClient so the consolidation pipeline
// can drive LLM-backed memory updates without the plugin interface depending on
// the Model class.
class HostMemoryModelClient : public MemoryModelClient
{
public:
    explicit HostMemoryModelClient(Model* model) : model_(model) {}

    MemoryModelResult GenerateMemoryUpdate(const std::string& prompt) override
    {
        MemoryModelResult result;
        if (!model_) {
            result.errorCode = "null_model";
            result.errorMessage = "No model provided";
            return result;
        }
        Message userMsg;
        userMsg.role = "user";
        userMsg.content = prompt;
        std::string formatted = model_->Format(
            "Follow the instructions in the user message precisely.", {userMsg}, {});
        ModelResponse response = model_->Invoke(formatted, nullptr);
        result.text = response.content;
        result.httpStatus = 200;
        if (response.content.empty()) {
            result.errorCode = "empty_response";
            result.errorMessage = "Model returned empty content";
        }
        return result;
    }

private:
    Model* model_;
};

} // namespace

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

    // workerEnv_ is now injected by SessionManager via SetWorkerEnv after
    // construction, eliminating the WorkerEnv→Agent back-reference.
    // Session-scoped resources (todoList, askUser) are owned by
    // SessionEntry, not by Agent.

    worker_ = CreateAgentWorker(this->config_);
    if (workerEnv_) {
        worker_->SetWorkerEnv(workerEnv_);
    }
    if (skillEngine_) {
        worker_->SetSkillEngine(skillEngine_);
    }

    historyStore_ = std::make_unique<HistoryStore>(dataPath);

    config_.dreamConfig.dataBasePath = dataPath;
    longTermConsolidator_ = std::make_unique<LegacyDreamConsolidator>(config_.dreamConfig);

    consolidationThread_ = std::thread(&Agent::ConsolidationLoop, this);

    LOG(INFO) << "[Agent] Single-Agent initialized with Dream memory consolidation, maxConcurrentSessions="
              << config_.maxConcurrentSessions;
}

Agent::~Agent()
{
    Shutdown();
    LOG(INFO) << "[Agent] Shutdown complete";
}

void Agent::Shutdown()
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
}

void Agent::SetContextEngineGetter(
    std::function<std::shared_ptr<ContextEngine>(const std::string&)> getter)
{
    contextEngineGetter_ = getter;
}

MemoryRuntime* Agent::GetMemoryRuntime()
{
    return memoryRuntime_;
}

void Agent::SetMemoryRuntime(MemoryRuntime* runtime)
{
    memoryRuntime_ = runtime;
}

void Agent::SetWorkerEnv(WorkerEnv* env)
{
    workerEnv_ = env;
    if (worker_) {
        worker_->SetWorkerEnv(workerEnv_);
    }
}

std::string Agent::Invoke(const std::string& sessionId, const std::string& query,
                          std::function<void(const std::string&)> callback)
{
    LOG(INFO) << "[Agent] Invoke started for session: " << sessionId;

    if (!worker_) {
        callback("[STATUS] Error: Agent not initialized");
        LOG(ERR) << "[Agent] [" << sessionId << "] Error: Agent not initialized";
        return "Error: Agent not initialized";
    }

    auto contextEngine = contextEngineGetter_(sessionId);
    if (!contextEngine) {
        callback("[STATUS] Error: ContextEngine not found for session=" + sessionId);
        LOG(ERR) << "[Agent] [" << sessionId << "] Error: ContextEngine not found";
        return "Error: Session context not found";
    }

    NotifySessionActive(sessionId);

    {
        Message userMsg;
        userMsg.role = "user";
        userMsg.content = query;
        contextEngine->AddMessage(userMsg);
    }
    if (historyStore_ && !memoryRuntime_) {
        historyStore_->AppendEntry("user", query, sessionId);
    }

    std::string finalAnswer = worker_->Invoke(
        query, contextEngine.get(),
        [callback](const std::string& response) {
            if (!response.empty() && callback) {
                callback(response);
            }
        });

    if (!finalAnswer.empty()) {
        if (historyStore_ && !memoryRuntime_) {
            historyStore_->AppendEntry("assistant", finalAnswer, sessionId);
        }
    }

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
    if (it == sessionActivity_.end()) {
        return false;
    }
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

int Agent::SyncMcpTools()
{
    auto currentSet = ResourceManager::GetInstance().GetMcpToolNames();
    std::unordered_set<std::string> desired(currentSet.begin(), currentSet.end());

    std::vector<std::string> toAdd;
    for (const auto& name : currentSet) {
        bool owned = std::find(ownedMcpTools_.begin(), ownedMcpTools_.end(), name) != ownedMcpTools_.end();
        if (!owned) {
            toAdd.push_back(name);
        }
    }

    std::vector<std::string> toRemove;
    for (const auto& name : ownedMcpTools_) {
        if (desired.find(name) == desired.end()) {
            toRemove.push_back(name);
        }
    }

    for (const auto& name : toAdd) {
        if (std::find(toolNames_.begin(), toolNames_.end(), name) == toolNames_.end()) {
            toolNames_.push_back(name);
        }
        ownedMcpTools_.push_back(name);
    }
    if (!toAdd.empty() && worker_) {
        worker_->AddTools(toAdd);
    }

    for (const auto& name : toRemove) {
        toolNames_.erase(std::remove(toolNames_.begin(), toolNames_.end(), name), toolNames_.end());
        ownedMcpTools_.erase(std::remove(ownedMcpTools_.begin(), ownedMcpTools_.end(), name), ownedMcpTools_.end());
    }
    if (!toRemove.empty() && worker_) {
        worker_->RemoveTools(toRemove);
    }

    return static_cast<int>(toAdd.size() + toRemove.size());
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
            if (idleSeconds <= 0) {
                idleSeconds = 60;
            }

            cv_.wait_for(lock, std::chrono::seconds(idleSeconds), [this]() {
                return !running_;
            });
            if (!running_) {
                break;
            }
        }

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

        if (!anyIdle) {
            continue;
        }

        try {
            auto model = ResourceManager::GetInstance().CreateModel(config_.modelConfig);
            bool handledByMemoryRuntime = false;
            if (memoryRuntime_) {
                MemoryConsolidationRequest request;
                request.agentId = config_.id;
                request.forceReprocess = false;
                HostMemoryModelClient hostClient(model.get());
                handledByMemoryRuntime = memoryRuntime_->Consolidate(request, &hostClient);
                if (handledByMemoryRuntime) {
                    LOG(INFO) << "[MemoryRuntime] Memory consolidation completed";
                }
            }

            if (!handledByMemoryRuntime) {
                bool didWork = longTermConsolidator_->Run(model.get(), historyStore_.get());
                if (didWork) {
                    LOG(INFO) << "[Dream] Memory consolidation completed";
                }
            }
        } catch (const std::exception& e) {
            LOG(WARN) << "[Memory] Consolidation failed: " << e.what();
        }
    }
}

std::vector<Skill> Agent::ListSkills() const
{
    if (!skillEngine_) {
        return {};
    }
    return skillEngine_->GetAllSkills();
}

Skill Agent::GetSkill(const std::string& id) const
{
    if (!skillEngine_) {
        return Skill{};
    }
    return skillEngine_->GetSkill(id);
}

std::string Agent::GetSkillRootDir() const
{
    if (!skillEngine_) {
        return {};
    }
    return skillEngine_->GetRootDir();
}

} // namespace jiuwen

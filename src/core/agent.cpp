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
#include "src/core/ask_user_dispatcher.h"
#include "src/core/dream_processor.h"
#include "src/core/history_store.h"
#include "src/core/session_todo_list.h"
#include "src/core/worker_env.h"
#include "src/skills/skill_engine.h"
#include "src/tools/builtin_tools/skill_search_tool.h"
#include "src/utils/data_dir.h"
#include "src/utils/logger.h"

namespace fs = std::filesystem;

namespace jiuwen {

// Private adapter that exposes the per-session resources Agent owns to the
// AgentWorker without leaking the full Agent type into the worker layer.
class WorkerEnvImpl : public WorkerEnv {
public:
    explicit WorkerEnvImpl(Agent* agent) : agent_(agent) {}

    SessionTodoList* GetOrCreateSessionTodoList(const std::string& sessionId) override
    {
        return agent_->GetOrCreateSessionTodoList(sessionId);
    }

    AskUserDispatcher* GetAskUserDispatcher() override
    {
        return agent_->GetAskUserDispatcher();
    }
private:
    Agent* agent_;
};

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

    askUserDispatcher_ = std::make_unique<AskUserDispatcher>();
    workerEnv_ = std::make_unique<WorkerEnvImpl>(this);

    worker_ = CreateAgentWorker(this->config_);
    if (worker_) {
        worker_->SetWorkerEnv(workerEnv_.get());
    }
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

    // 1. Mark session as active
    NotifySessionActive(sessionId);

    // 2. Save user query to context (worker will see it via GetContextWindow).
    {
        Message userMsg;
        userMsg.role = "user";
        userMsg.content = query;
        contextEngine->AddMessage(userMsg);
    }
    if (historyStore_) {
        historyStore_->AppendEntry("user", query, sessionId);
    }

    // 3. Run the worker. The worker is now responsible for persisting any
    // assistant tool_calls and tool result messages into the ContextEngine
    // directly; this Agent layer only handles user/final-assistant turns.
    std::string finalAnswer = worker_->Invoke(
        query, contextEngine.get(),
        [callback](const std::string& response) {
            if (!response.empty() && callback) {
                callback(response);
            }
        });

    // 5. Save final assistant text into Dream's history store. The
    // ContextEngine already received the structured assistant message from
    // the worker (or a tool_calls-only assistant; either way we don't
    // double-write here).
    if (!finalAnswer.empty()) {
        if (historyStore_) {
            historyStore_->AppendEntry("assistant", finalAnswer, sessionId);
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

    // Compute additions: in 'desired' but not yet in ownedMcpTools_
    std::vector<std::string> toAdd;
    for (const auto& name : currentSet) {
        bool owned = std::find(ownedMcpTools_.begin(), ownedMcpTools_.end(), name) != ownedMcpTools_.end();
        if (!owned) {
            toAdd.push_back(name);
        }
    }

    // Compute removals: in ownedMcpTools_ but no longer in 'desired'
    std::vector<std::string> toRemove;
    for (const auto& name : ownedMcpTools_) {
        if (desired.find(name) == desired.end()) {
            toRemove.push_back(name);
        }
    }

    // Apply additions: extend Agent's master list + worker pool
    for (const auto& name : toAdd) {
        if (std::find(toolNames_.begin(), toolNames_.end(), name) == toolNames_.end()) {
            toolNames_.push_back(name);
        }
        ownedMcpTools_.push_back(name);
    }
    if (!toAdd.empty() && worker_) {
        worker_->AddTools(toAdd);
    }

    // Apply removals: shrink Agent's master list + worker pool
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

        if (!anyIdle) {
            continue;
        }

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

SessionTodoList* Agent::GetOrCreateSessionTodoList(const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(sessionTodosMutex_);
    auto it = sessionTodos_.find(sessionId);
    if (it == sessionTodos_.end()) {
        auto list = std::make_unique<SessionTodoList>();
        auto* raw = list.get();
        sessionTodos_[sessionId] = std::move(list);
        return raw;
    }
    return it->second.get();
}

AskUserDispatcher* Agent::GetAskUserDispatcher()
{
    return askUserDispatcher_.get();
}

bool Agent::ProvideUserResponse(const std::string& requestId, const std::string& answer)
{
    if (!askUserDispatcher_) {
        return false;
    }
    return askUserDispatcher_->ProvideResponse(requestId, answer);
}

} // namespace jiuwen

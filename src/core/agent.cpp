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
        // SkillEngine is no longer wired to SkillSearchTool via a global
        // static pointer; it is injected per-invoke via ToolBuildContext
        // (ctx.skillEngine), resolved by WorkerEnv from the active Agent.
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

    // HistoryStore is now owned by SessionManager (set via SetHistoryStore);
    // Agent holds a non-owning pointer that survives ReloadAgent.

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

void Agent::SetHistoryStore(HistoryStore* store)
{
    historyStore_ = store;
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
    // RAII: ensure NotifySessionIdle runs on ALL exit paths (normal return,
    // exception from AddMessage's sink / worker, or early return). Without
    // this, an exception would leak sessionActivity_[sid].isBusy=true forever,
    // making ConsolidationLoop never consider the session idle.
    struct SessionIdleGuard {
        Agent* self;
        std::string sid;
        bool armed{true};
        ~SessionIdleGuard() { if (armed) self->NotifySessionIdle(sid); }
    } idleGuard{this, sessionId};

    // Persist the user message via ContextEngine. The event sink (set by
    // SessionManager per session) routes to MemoryRuntime if configured, or
    // to HistoryStore (the local fallback) otherwise. Agent no longer makes
    // the persistence-routing decision -- it is a pure Facade for this step.
    {
        Message userMsg;
        userMsg.role = "user";
        userMsg.content = query;
        contextEngine->AddMessage(userMsg);
    }

    // The worker adds intermediate assistant/tool messages and the final
    // answer to ContextEngine (which feeds the same sink), so the full event
    // stream reaches the configured persistence target without Agent
    // writing to HistoryStore directly.
    std::string finalAnswer = worker_->Invoke(
        query, contextEngine.get(),
        [callback](const std::string& response) {
            if (!response.empty() && callback) {
                callback(response);
            }
        });

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
    bool isExcluded = false;
    {
        // Quick snapshot of the excluded set so we don't hold
        // sessionActivityMutex_ across config reads. config_ is a value
        // member initialized once at Agent construction and never mutated
        // afterwards -- ReloadAgent swaps the whole Agent instance (new
        // thread, new config_), it does NOT rewrite config_ in place.
        // This is what makes the lock-free read here safe.
        //
        // IMPORTANT: any future feature that makes config_ mutable within
        // an Agent's lifetime (e.g. ConfigWatcher hot-updating
        // excludedConsolidationSessionIds) MUST audit this read together
        // with sessionActivityMutex_ acquisition -- a concurrent writer
        // would turn this into a data race.
        const auto& excluded = config_.memoryConfig.excludedConsolidationSessionIds;
        if (!excluded.empty()) {
            isExcluded = std::find(excluded.begin(), excluded.end(), sessionId) != excluded.end();
        }
    }

    std::lock_guard<std::mutex> lock(sessionActivityMutex_);
    auto it = sessionActivity_.find(sessionId);
    if (it != sessionActivity_.end()) {
        it->second.isBusy = false;
    }
    // System-triggered sessions (cron / heartbeat / application-declared
    // reserved sessions) must not arm the consolidation dirty flag: their
    // events are excluded from the batch by config, and letting them wake
    // ConsolidationLoop would defeat the "skip CreateModel when no new user
    // conversation finished" optimization. We still update isBusy above so
    // the anyIdle gate and busy tracking behave normally.
    if (isExcluded) {
        // No cv_ signal here: ConsolidationLoop is poll-driven -- it wakes
        // every idleConsolidationSeconds via wait_for timeout (or on
        // Shutdown's notify) and picks up idle sessions from
        // sessionActivity_ on each poll.
        return;
    }
    // Mark that a conversation just completed. By the time NotifySessionIdle
    // runs (RAII guard in Invoke), AddMessage has already routed the
    // session's events into the memory store, so the cursor will find new
    // work. This lets ConsolidationLoop skip CreateModel + the cursor query
    // when no conversation has finished since the last consolidation pass.
    hasNewActivity_.store(true, std::memory_order_release);
    // No cv_ signal here: ConsolidationLoop is poll-driven -- it wakes every
    // idleConsolidationSeconds via wait_for timeout (or on Shutdown's
    // notify) and picks up idle sessions from sessionActivity_ on each
    // poll. A notify here would be a no-op (the wait predicate only checks
    // !running_) and would be signalled under the wrong mutex
    // (sessionActivityMutex_ L4 vs the wait's consolidationMutex_ L3).
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

void Agent::CleanupSession(const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(sessionActivityMutex_);
    sessionActivity_.erase(sessionId);
}

void Agent::Cancel()
{
    if (worker_) {
        worker_->Cancel();
    }
}

void Agent::AddTools(const std::vector<std::string>& toolNames)
{
    // Tool state lives in AgentWorker (single source of truth, under
    // toolMutex_); Agent is a thin proxy. The worker dedups and validates
    // against ResourceManager.
    if (worker_) {
        worker_->AddTools(toolNames);
    }
}

int Agent::SyncMcpTools()
{
    // MCP ownership diff + toolNames_ reconciliation lives in AgentWorker
    // (atomic under toolMutex_). Agent is a thin proxy.
    if (worker_) {
        return worker_->SyncMcpTools();
    }
    return 0;
}

std::vector<std::string> Agent::GetRegisteredTools() const
{
    if (worker_) {
        return worker_->GetToolNames();
    }
    return {};
}

void Agent::ConsolidationLoop()
{
    // Catch-up: the first iteration bypasses both the anyIdle and
    // hasNewActivity_ gates so a freshly constructed (or hot-reloaded) Agent
    // picks up pending events from the cursor without waiting for a new
    // conversation to complete. After the first iteration, normal gating
    // resumes. ReloadAgent constructs a new Agent (new thread, new local),
    // so firstCycle resets naturally on each reload.
    bool firstCycle = true;

    while (running_) {
        {
            std::unique_lock<std::mutex> lock(consolidationMutex_);
            auto idleSeconds = static_cast<unsigned int>(config_.memoryConfig.idleConsolidationSeconds);
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

        // Normal gating (skipped on the first iteration for catch-up).
        if (!firstCycle) {
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

            // Activity gate: skip the entire consolidation attempt (CreateModel +
            // cursor query) when no conversation has finished since the last pass.
            // hasNewActivity_ is set by NotifySessionIdle and cleared below before
            // driving Consolidate (clear-before). If a new conversation completes
            // during Consolidate, NotifySessionIdle re-arms the flag and the next
            // pass picks it up -- the cursor mechanism keeps the re-entry idempotent.
            if (!hasNewActivity_.load(std::memory_order_acquire)) {
                continue;
            }
        }
        firstCycle = false;

        try {
            // Clear before Consolidate: any activity arriving during the
            // consolidation pass re-arms the flag (see NotifySessionIdle) and
            // is handled on the next pass. The cursor is the correctness
            // boundary, so a missed retry here only delays work by one cycle.
            hasNewActivity_.store(false, std::memory_order_release);

            auto model = ResourceManager::GetInstance().CreateModel(config_.modelConfig);
            bool handledByMemoryRuntime = false;
            if (memoryRuntime_) {
                MemoryConsolidationRequest request;
                request.agentId = config_.id;
                request.forceReprocess = false;
                // Skip system-triggered sessions (cron / heartbeat / any
                // application-declared reserved id) so the LLM only sees
                // real user conversations. Events from these sessions are
                // still persisted for audit and still advance the cursor.
                request.excludedSessionIds = config_.memoryConfig.excludedConsolidationSessionIds;
                HostMemoryModelClient hostClient(model.get());
                handledByMemoryRuntime = memoryRuntime_->Consolidate(request, &hostClient);
                if (handledByMemoryRuntime) {
                    LOG(INFO) << "[MemoryRuntime] Memory consolidation completed";
                }
            }

            if (!handledByMemoryRuntime) {
                bool didWork = longTermConsolidator_->Run(model.get(), historyStore_);
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

SkillEngine* Agent::GetSkillEngine() const
{
    return skillEngine_.get();
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

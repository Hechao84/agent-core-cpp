#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "include/agent_export.h"
#include "include/memory_runtime.h"
#include "include/skill.h"
#include "include/types.h"

namespace jiuwen {

class AgentWorker;
class ContextEngine;
class SkillEngine;
class HistoryStore;
class LongTermConsolidator;
class WorkerEnv;

struct SessionActivity
{
    std::string sessionId;
    bool isBusy{false};
};

class AGENT_API Agent {
public:
    Agent(AgentConfig config);
    ~Agent();

    std::string Invoke(const std::string& sessionId, const std::string& query,
                       std::function<void(const std::string&)> callback);
    bool IsSessionBusy(const std::string& sessionId) const;
    void Cancel();

    // 退役标志：由 SessionManager::ReloadAgent 在新建替换 Agent 后对旧 Agent 置位。
    // draining 的 Agent 不再接新 session 的在途回合（Invoke 路由会重绑到活跃 Agent），
    // 但继续服务已绑定它的存量 session 的在途 Invoke 直至自然跑完。仅 advisory
    // 路由提示，不改变 Invoke 内部行为。
    void MarkDraining() { draining_.store(true, std::memory_order_release); }
    bool IsDraining() const { return draining_.load(std::memory_order_acquire); }

    // Stop background work (the consolidation thread) and join it. Idempotent:
    // safe to call multiple times and is also invoked by the destructor. Lets
    // the application drain the agent gracefully on shutdown without relying on
    // the (possibly externally referenced) shared_ptr's destruction timing.
    void Shutdown();

    void AddTools(const std::vector<std::string>& toolNames);
    std::vector<std::string> GetRegisteredTools() const;

    std::vector<Skill> ListSkills() const;
    Skill GetSkill(const std::string& id) const;
    std::string GetSkillRootDir() const;
    // Non-owning access to the Agent's SkillEngine, used by WorkerEnv to
    // inject into ToolBuildContext for skill_search (resolves fresh each
    // call so it tracks ReloadAgent swaps).
    SkillEngine* GetSkillEngine() const;

    const std::vector<std::string>& GetMcpServerIds() const { return config_.mcpServerIds; }
    int SyncMcpTools();

    void NotifySessionIdle(const std::string& sessionId);
    void NotifySessionActive(const std::string& sessionId);

    // Erase session-scoped state from Agent::sessionActivity_ after the
    // session is removed from SessionManager. Called by RemoveSession
    // outside sessionMutex_ to avoid lock-ordering violations.
    void CleanupSession(const std::string& sessionId);

    // Non-owning memory runtime shared across sessions. Ownership lives in
    // SessionManager so the runtime (and the ContextEngine callbacks that
    // capture it) survive an Agent hot-reload.
    void SetMemoryRuntime(MemoryRuntime* runtime);
    // HistoryStore is now owned by SessionManager (survives ReloadAgent, like
    // memoryRuntime_); Agent holds a non-owning pointer set via SetHistoryStore.
    void SetHistoryStore(HistoryStore* store);

    // Inject the WorkerEnv that resolves session-scoped resources via
    // SessionManager. Called by SessionManager after Agent construction
    // eliminates WorkerEnv→Agent back-reference.
    void SetWorkerEnv(WorkerEnv* env);
private:
    AgentConfig config_;
    std::unique_ptr<AgentWorker> worker_;
    std::shared_ptr<SkillEngine> skillEngine_;

    std::function<std::shared_ptr<ContextEngine>(const std::string&)> contextEngineGetter_;

    void SetContextEngineGetter(std::function<std::shared_ptr<ContextEngine>(const std::string&)> getter);
    MemoryRuntime* GetMemoryRuntime();
    friend class SessionManager;

    std::thread consolidationThread_;
    mutable std::mutex consolidationMutex_;  // Lock layer L3 (consolidation thread CV)
    std::condition_variable cv_;
    std::atomic<bool> running_{true};

    // 退役标志：跨 Agent 对象实例的读写（写在 ReloadAgent 持 sessionMutex_、
    // 读在 Invoke 持 sessionMutex_），故必须原子。仅 advisory 路由提示。
    std::atomic<bool> draining_{false};

    HistoryStore* historyStore_{nullptr};  // non-owning, owned by SessionManager
    std::unique_ptr<LongTermConsolidator> longTermConsolidator_;
    // NON-OWNING. Owned by SessionManager::memoryRuntime_, which outlives every
    // Agent (it is created once and reused across hot-reloads, and destroyed
    // only after all Agents are torn down). Must never be deleted here. See the
    // lifetime contract on SessionManager::memoryRuntime_.
    MemoryRuntime* memoryRuntime_{nullptr};

    // NON-OWNING. Owned by SessionManager::workerEnv_, injected after Agent
    // construction. The worker resolves session-scoped resources (todoList,
    // askUser, memoryRuntime) via this interface without back-referencing
    // Agent (cycle elimination).
    WorkerEnv* workerEnv_{nullptr};

    mutable std::mutex sessionActivityMutex_;  // Lock layer L4 (session activity tracking)
    std::unordered_map<std::string, SessionActivity> sessionActivity_;

    // Activity gate for ConsolidationLoop: set by NotifySessionIdle when a
    // conversation completes (events have been appended to the store), cleared
    // by ConsolidationLoop right before driving Consolidate. A purely advisory
    // performance hint — the memory runtime's cursor mechanism remains the
    // source of truth for idempotency. Lets the loop skip CreateModel + the
    // cursor query entirely when no new conversation has finished since the
    // last consolidation pass.
    std::atomic<bool> hasNewActivity_{false};

    void ConsolidationLoop();
};

} // namespace jiuwen

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
class SessionTodoList;
class AskUserDispatcher;
class WorkerEnvImpl;

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

    const std::vector<std::string>& GetMcpServerIds() const { return config_.mcpServerIds; }
    int SyncMcpTools();

    // Resolve a pending ask_user request. Called by application-layer
    // adapters (HTTP, CLI, channel bridges) after they collected the user's
    // answer for a particular request id emitted via the [ASK_USER] tag.
    // Returns true if the request id matched a pending slot.
    bool ProvideUserResponse(const std::string& requestId, const std::string& answer);

    void NotifySessionIdle(const std::string& sessionId);
    void NotifySessionActive(const std::string& sessionId);

    // Non-owning memory runtime shared across sessions. Ownership lives in
    // SessionManager so the runtime (and the ContextEngine callbacks that
    // capture it) survive an Agent hot-reload.
    void SetMemoryRuntime(MemoryRuntime* runtime);
private:
    AgentConfig config_;
    std::unique_ptr<AgentWorker> worker_;
    std::shared_ptr<SkillEngine> skillEngine_;
    std::vector<std::string> toolNames_;
    std::vector<std::string> ownedMcpTools_;

    std::function<std::shared_ptr<ContextEngine>(const std::string&)> contextEngineGetter_;

    void SetContextEngineGetter(std::function<std::shared_ptr<ContextEngine>(const std::string&)> getter);
    MemoryRuntime* GetMemoryRuntime();
    friend class SessionManager;

    std::thread consolidationThread_;
    mutable std::mutex consolidationMutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};

    std::unique_ptr<HistoryStore> historyStore_;
    std::unique_ptr<LongTermConsolidator> longTermConsolidator_;
    // NON-OWNING. Owned by SessionManager::memoryRuntime_, which outlives every
    // Agent (it is created once and reused across hot-reloads, and destroyed
    // only after all Agents are torn down). Must never be deleted here. See the
    // lifetime contract on SessionManager::memoryRuntime_.
    MemoryRuntime* memoryRuntime_{nullptr};

    // Session-scoped resources owned by Agent and accessed by AgentWorker
    // through the private WorkerEnv adapter.
    std::unordered_map<std::string, std::unique_ptr<SessionTodoList>> sessionTodos_;
    mutable std::mutex sessionTodosMutex_;
    std::unique_ptr<AskUserDispatcher> askUserDispatcher_;
    std::unique_ptr<WorkerEnvImpl> workerEnv_;

    SessionTodoList* GetOrCreateSessionTodoList(const std::string& sessionId);
    AskUserDispatcher* GetAskUserDispatcher();
    friend class WorkerEnvImpl;

    mutable std::mutex sessionActivityMutex_;
    std::unordered_map<std::string, SessionActivity> sessionActivity_;

    void ConsolidationLoop();
};

} // namespace jiuwen

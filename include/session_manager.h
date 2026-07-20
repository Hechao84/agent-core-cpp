#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "include/agent_export.h"
#include "include/memory_runtime.h"
#include "include/model.h"
#include "include/types.h"

namespace jiuwen {

class Agent;
class ContextEngine;
class SessionTodoList;
class AskUserDispatcher;
class AskUserRouter;
class WorkerEnv;
class HistoryStore;
class ToolTurnState;  // Per-turn disclosure state proxy (full def in src/core/tool_turn_state.h; internal, not exported)

// Reserved session id used by the core library as the MakeSessionKey fallback
// when both channel and chatId are empty. Application layers should define
// their own default session constant rather than reusing this one.
//
// The former kHeartbeatSessionId / kCronSessionId constants have been moved
// to the application layer (e.g. examples/jiuwenClaw/reserved_sessions.h).
// The core library no longer knows about cron / heartbeat semantics; instead,
// applications register their own reserved sessions via
// SessionManager::RegisterReservedSession and declare them via
// MemoryConfig::excludedConsolidationSessionIds so they are excluded from
// memory consolidation.
inline constexpr char kDefaultSessionId[] = "__DEFAULT__";

struct SessionEntry
{
    std::string sessionId;
    std::shared_ptr<ContextEngine> contextEngine;
    std::unique_ptr<SessionTodoList> todoList;
    std::unique_ptr<AskUserDispatcher> askUser;
    // 该 session 绑定的 Agent；ReloadAgent 后旧 Agent 由本引用保活，继续服务
    // 该 session 的在途回合直至自然跑完。存量 session 的下一条新消息会因旧
    // Agent 处于 draining 而重新绑定到当前活跃 Agent。与 contextEngine /
    // todoList / askUser 同属会话级状态，生命周期跟随 SessionEntry（由
    // shared_ptr 管理，跨 RemoveSession 的 in-flight 引用兜底）。
    std::shared_ptr<Agent> agent;
    std::mutex invokeMutex; // Lock layer L3 (per-session call serialization)
    std::atomic<bool> isBusy{false};
    std::map<std::string, std::string> metadata; // channel, sender, etc.

    // Progressive tool disclosure per-turn state (round5 design §5.2).
    // activeSet/loadedTools are cleared at the start of each Invoke (per-turn
    // reset) while the SessionEntry itself persists across turns (per-session
    // host). turnStateProxy is the ToolTurnState implementation that forwards
    // load/search/getActiveSet/getLoadedTools to these two sets; lazily
    // constructed once per SessionEntry by SmWorkerEnv::GetCurrentTurnState()
    // and stable thereafter (only the set contents reset, not the proxy).
    std::set<std::string> activeSet;
    std::set<std::string> loadedTools;
    std::unique_ptr<ToolTurnState> turnStateProxy;

    // Defined out-of-line in the .cpp (where ToolTurnState is complete) so
    // the std::unique_ptr<ToolTurnState> member can be destroyed without the
    // full type definition leaking into this public header (tool_turn_state.h
    // is an internal header under src/core/ — only forward-declared here).
    ~SessionEntry();
};

class AGENT_API SessionManager
{
public:
    SessionManager();
    ~SessionManager();

    // Initialize with configuration (must be called once before any Invoke)
    void Initialize(const AgentConfig& config);

    // Invoke a session with the given message. Thread-safe.
    // If session does not exist, it will be created on first use.
    // callback is called with streaming tokens/status/tool events.
    SessionInvokeResult Invoke(
        const std::string& sessionId,
        const std::string& message,
        std::function<void(const std::string&)> callback
    );

    // Same as Invoke but from a ChannelMessage (auto-derives sessionId)
    SessionInvokeResult InvokeChannel(
        const ChannelMessage& msg,
        std::function<void(const std::string&)> callback
    );

    // Cancel the running agent
    void Cancel();

    // Gracefully stop background work before process exit. Stops the Agent's
    // consolidation thread (Agent::Shutdown) so it is joined deterministically
    // rather than at static teardown. Idempotent. Does NOT delete the singleton
    // itself: the instance is intentionally never freed (see InitSessionManager)
    // so any reference handed out by GetSessionManager() never dangles; its
    // memory is reclaimed by the OS at process exit. Callers must ensure all
    // threads that may call into SessionManager (heartbeat, cron, channels,
    // HTTP) are stopped before invoking this.
    void Shutdown();

    // Remove a session by ID and delete its data
    void RemoveSession(const std::string& sessionId);

    // Check if a session is busy
    bool IsSessionBusy(const std::string& sessionId) const;

    // Get or create a session by ID (returns ContextEngine)
    std::shared_ptr<ContextEngine> GetOrCreateSession(
        const std::string& sessionId,
        const SessionConfig& sessionConfig = {}
    );

    // Get all messages of a session. Returns empty vector if session does not exist.
    std::vector<Message> GetSessionMessages(const std::string& sessionId) const;

    // Get all active session IDs
    std::vector<std::string> GetSessionIds() const;

    // Get session metadata
    std::map<std::string, std::string> GetSessionMetadata(const std::string& sessionId) const;

    // Check if initialized
    bool IsInitialized() const { return initialized_; }

    // Get the global AgentConfig
    const AgentConfig& GetConfig() const { return config_; }

    // Access the live Agent (e.g. to enumerate its skills via
    // Agent::ListSkills()). Returns an empty shared_ptr before Initialize.
    //
    // The returned shared_ptr keeps the underlying Agent alive for the
    // caller's full use, even if ReloadAgent runs concurrently and swaps
    // in a new Agent (the old Agent is marked draining rather than
    // cancelled, and is destroyed only after the last shared_ptr to it is
    // released). The agent_ read is guarded by sessionMutex_ (the same lock
    // ReloadAgent swaps agent_ under) so the shared_ptr copy cannot race
    // with a reload. Callers should hold the shared_ptr only for the
    // duration of a single request/command.
    std::shared_ptr<Agent> GetAgent() const
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        return agent_;
    }

    // Access the shared memory runtime owned by SessionManager. Returns
    // nullptr when memory is disabled or initialization failed. The runtime is
    // shared across all sessions and survives an Agent hot-reload.
    MemoryRuntime* GetMemoryRuntime() const { return memoryRuntime_.get(); }

    // Access the local fallback HistoryStore owned by SessionManager. Always
    // non-null after Initialize. Used as the ContextEngine event sink target
    // when no MemoryRuntime is configured (the local simplified MemoryRuntime
    // fallback). Survives an Agent hot-reload (like memoryRuntime_).
    HistoryStore* GetHistoryStore() const { return historyStore_.get(); }

    // 优雅退役式热重载：构造新 Agent → 设为活跃（后续新 session 绑它）→
    // 旧 Agent 标记 draining（不再接新 session 的在途回合，继续服务已绑定
    // 它的存量 session 直至跑完）。不 drain、不 Cancel 旧 Agent，运维完全
    // 不阻塞。旧 Agent 的析构由绑定它的 SessionEntry 的 shared_ptr 引用
    // 计数驱动。Existing sessions (history/context) are preserved; in-flight
    // calls are NOT interrupted — they keep running on the (now draining) old
    // Agent until the turn completes. Returns false if the new Agent could
    // not be constructed - the old Agent stays in place in that case. On
    // false, 'errorOut' (when non-null) receives a diagnostic.
    bool ReloadAgent(const AgentConfig& newConfig, std::string* errorOut = nullptr);

    // Generate a session key from channel + chatId
    static std::string MakeSessionKey(const std::string& channel, const std::string& chatId);

    // Register a session id as reserved: it cannot be deleted via
    // RemoveSession. The core library auto-registers kDefaultSessionId at
    // construction; application layers register their own system session
    // ids (e.g. cron / heartbeat) at startup. Reserved status is orthogonal
    // to MemoryConfig::excludedConsolidationSessionIds: the former protects
    // a session from deletion, the latter steers memory consolidation.
    void RegisterReservedSession(std::string id);

    // Resolve a pending ask_user request by requestId. Routes the answer
    // to the correct session's AskUserDispatcher regardless of whether
    // the Agent has been hot-reloaded. Returns true if the requestId
    // matched a pending slot.
    bool ProvideUserResponse(const std::string& requestId, const std::string& answer);

private:
    std::shared_ptr<SessionEntry> FindOrCreateEntry(const std::string& sessionId);
    std::shared_ptr<ContextEngine> GetContextEngine(const std::string& sessionId);
    void SetupAgentContextRouting();
    void InitMemoryRuntime();

    friend class SmWorkerEnv;
    friend class SmAskUserRouter;

    AgentConfig config_;
    bool initialized_{false};

    // Reserved session ids (cannot be deleted via RemoveSession). Always
    // contains kDefaultSessionId (auto-registered by the constructor) plus
    // any application-registered ids. Guarded by reservedMutex_; only
    // non-const methods (RegisterReservedSession / RemoveSession) touch
    // this set, so a plain std::mutex suffices (no mutable needed).
    std::mutex reservedMutex_;
    std::unordered_set<std::string> reservedSessions_;

    // Shared memory runtime, owned here (not by Agent) so it outlives an
    // Agent hot-reload and keeps the ContextEngine callbacks that capture it
    // valid across a swap.
    //
    // LIFETIME CONTRACT: memoryRuntime_ is the sole owner. Several parties hold
    // NON-OWNING raw MemoryRuntime* into it: Agent::memoryRuntime_, the
    // ContextEngine memory callbacks (captured per session), ToolBuildContext /
    // MemoryReadPayloadTool, and WorkerEnv::GetMemoryRuntime(). All of these
    // must not outlive memoryRuntime_. This is guaranteed two ways:
    //   1. Declaration order: memoryRuntime_ is declared BEFORE agent_ and
    //      sessions_, so it is destroyed AFTER them (members destruct in
    //      reverse declaration order). DO NOT reorder these members.
    //   2. ~SessionManager() tears down agent_/sessions_ first (Shutdown()
    //      only joins the consolidation thread + curl cleanup; it does not
    //      reset members).
    // memoryRuntime_ itself is created once in Initialize() and never rebuilt
    // (ReloadAgent reuses it), so the raw pointers stay valid across reloads.
    std::unique_ptr<MemoryRuntime> memoryRuntime_;

    // Local simplified MemoryRuntime fallback. Always created in Initialize
    // (regardless of memoryConfig.enabled). When no MemoryRuntime is
    // configured, the per-session ContextEngine event sink routes here so the
    // full event stream is persisted locally for DreamProcessor. Same lifetime
    // contract as memoryRuntime_: declared before agent_/sessions_, never
    // rebuilt across reload, reset in ~SessionManager() after sessions_.clear()
    // (Shutdown() does not reset members).
    std::unique_ptr<HistoryStore> historyStore_;

    // Single shared Agent instance. shared_ptr (not unique_ptr) so that
    // GetAgent() can hand a strong reference to callers without risk of
    // dangling across a ReloadAgent swap.
    // NOTE: declared AFTER memoryRuntime_ on purpose (see lifetime contract).
    std::shared_ptr<Agent> agent_;

    // Per-session ContextEngine instances
    mutable std::mutex sessionMutex_;  // Lock layer L2 (session registry)
    std::unordered_map<std::string, std::shared_ptr<SessionEntry>> sessions_;

    // Global concurrency gate: optional maxConcurrentSessions limiter.
    // ReloadAgent no longer raises a reload barrier (graceful shutdown:
    // in-flight calls keep running on the draining old Agent). The gate
    // is now purely an optional concurrency cap.
    mutable std::mutex concurrencyMutex_;  // Lock layer L1 (global concurrency gate)
    std::condition_variable concurrencyCv_;
    int concurrentCount_{0};
    int maxConcurrent_{0};

    // requestId → sessionId index for routing ask_user responses.
    // Populated when AskUserDispatcher::EmitAskUser fires, cleared on
    // ProvideResponse or timeout (WaitForResponse cleanup).
    mutable std::mutex askIndexMutex_;  // Lock layer L4 (ask_user request routing)
    std::unordered_map<std::string, std::string> askRequestToSession_;

    // WorkerEnv implementation that resolves session-scoped resources
    // via SessionManager (eliminates WorkerEnv→Agent back-reference).
    // Defined in session_manager.cpp as a private adapter class.
    std::unique_ptr<WorkerEnv> workerEnv_;

    // AskUserRouter implementation that routes requestId registration
    // into the askRequestToSession_ index. Defined in session_manager.cpp.
    std::unique_ptr<AskUserRouter> askRouter_;
};

// Global singleton
AGENT_API SessionManager& GetSessionManager();
AGENT_API void InitSessionManager(const AgentConfig& config);

} // namespace jiuwen

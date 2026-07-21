#include "include/session_manager.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include "include/agent.h"
#include "include/resource_manager.h"
#include "src/context_engine/context_engine.h"
#include "src/core/agent_worker.h"
#include "src/core/ask_user_dispatcher.h"
#include "src/core/history_store.h"
#include "src/core/session_todo_list.h"
#include "src/core/turn_state_proxy.h"
#include "src/core/worker_env.h"
#include "src/tools/builtin_tools/tool_search_tool.h"
#include "src/utils/data_dir.h"
#include "src/utils/logger.h"
#include "src/utils/curl_client.h"

namespace fs = std::filesystem;

namespace jiuwen {

namespace {
    std::atomic<SessionManager*> g_sessionManager{nullptr};
    std::mutex g_initMutex;  // Lock layer L0 (singleton init)
}

// Out-of-line destructor: TurnState is complete in this TU (via the proxy
// header included above), so the unique_ptr<TurnState> member can be
// destroyed here without leaking the full type into the public header.
SessionEntry::~SessionEntry() = default;

// WorkerEnv adapter that resolves session-scoped resources through
// SessionManager. This eliminates the AgentWorker → WorkerEnv → Agent
// back-reference cycle: the worker only depends on the
// WorkerEnv interface, and this implementation routes via SessionManager
// which owns SessionEntry (todoList, askUser) and memoryRuntime.
class SmWorkerEnv : public WorkerEnv {
public:
    explicit SmWorkerEnv(SessionManager* sm) : sm_(sm) {}

    std::shared_ptr<ContextEngine> GetContextEngine(const std::string& sessionId) override
    {
        if (tlCurrentEntry_) {
            return tlCurrentEntry_->contextEngine;
        }
        return sm_->GetContextEngine(sessionId);
    }

    SessionTodoList* GetOrCreateSessionTodoList(const std::string& sessionId) override
    {
        if (tlCurrentEntry_) {
            if (!tlCurrentEntry_->todoList) {
                tlCurrentEntry_->todoList = std::make_unique<SessionTodoList>();
            }
            return tlCurrentEntry_->todoList.get();
        }
        std::lock_guard<std::mutex> lock(sm_->sessionMutex_);
        auto it = sm_->sessions_.find(sessionId);
        if (it == sm_->sessions_.end() || !it->second) {
            return nullptr;
        }
        if (!it->second->todoList) {
            it->second->todoList = std::make_unique<SessionTodoList>();
        }
        return it->second->todoList.get();
    }

    AskUserDispatcher* GetAskUserDispatcher(const std::string& sessionId) override
    {
        if (tlCurrentEntry_) {
            return tlCurrentEntry_->askUser.get();
        }
        std::lock_guard<std::mutex> lock(sm_->sessionMutex_);
        auto it = sm_->sessions_.find(sessionId);
        if (it == sm_->sessions_.end() || !it->second) {
            return nullptr;
        }
        return it->second->askUser.get();
    }

    MemoryRuntime* GetMemoryRuntime() override
    {
        return sm_->memoryRuntime_.get();
    }

    SkillEngine* GetSkillEngine() override
    {
        // During an Invoke, the cached SessionEntry holds the Agent bound to
        // THIS turn (entry->agent, set/rebound under sessionMutex_ before
        // SetCurrentEntry was called). Returning its SkillEngine upholds the
        // "one turn, one Agent" invariant: even if ReloadAgent swaps the
        // active agent_ mid-turn, this turn keeps using the bound (possibly
        // draining) Agent's SkillEngine — never the new Agent's.
        //
        // Reading tlCurrentEntry_->agent is safe without sessionMutex_: the
        // entry is held by a thread-local shared_ptr (only SetCurrentEntry /
        // ClearCurrentEntry on this same thread touch it), and entry->agent is
        // written only at Invoke entry (under sessionMutex_, before
        // SetCurrentEntry) and never mutated by ReloadAgent. No concurrent
        // writer exists during the turn.
        if (tlCurrentEntry_ && tlCurrentEntry_->agent) {
            return tlCurrentEntry_->agent->GetSkillEngine();
        }
        // Fallback (outside an Invoke, no cached entry): read the active
        // agent_ under the same lock ReloadAgent swaps under, so the
        // shared_ptr read does not race with a concurrent reload.
        std::lock_guard<std::mutex> lock(sm_->sessionMutex_);
        return sm_->agent_ ? sm_->agent_->GetSkillEngine() : nullptr;
    }

    // Per-thread pre-caching: the Invoke flow is SetCurrentEntry →
    // acquire invokeMutex → Invoke → ClearCurrentEntry, all on the
    // same thread. thread_local ensures each thread's cached entry
    // is independent, eliminating the data race that would exist
    // with a plain member (shared across all Invoke threads).
    void SetCurrentEntry(std::shared_ptr<SessionEntry> entry) override
    {
        tlCurrentEntry_ = std::move(entry);
    }

    void ClearCurrentEntry() override
    {
        tlCurrentEntry_.reset();
    }

    TurnState* GetCurrentTurnState() override
    {
        // Single-path (not GetSkillEngine()'s fast+slow double-path): turnState
        // is per-SessionEntry, and with no tlCurrentEntry_ there is no current
        // session to fall back to (no sessionId param either). Callers
        // (BuildToolSchemas, ExecuteTool's ctx fill) always run inside an
        // Invoke where tlCurrentEntry_ is set; a nullptr return covers the
        // off-Invoke probe/offline path only.
        if (!tlCurrentEntry_) {
            return nullptr;
        }
        // Lazily construct the proxy once per SessionEntry. Subsequent hits
        // return the stable proxy (per-turn reset clears activeSet/loadedTools
        // /skillActiveSet CONTENTS, the proxy object is NOT rebuilt). The raw
        // SessionEntry* back-pointer is safe: SessionEntry owns this
        // unique_ptr, so the proxy cannot outlive its host.
        if (!tlCurrentEntry_->turnStateProxy) {
            tlCurrentEntry_->turnStateProxy = std::make_unique<TurnStateProxy>(tlCurrentEntry_.get());
        }
        return tlCurrentEntry_->turnStateProxy.get();
    }

private:
    SessionManager* sm_;
    static thread_local std::shared_ptr<SessionEntry> tlCurrentEntry_;
};

thread_local std::shared_ptr<SessionEntry> SmWorkerEnv::tlCurrentEntry_;

// AskUserRouter adapter: routes registration calls from AskUserDispatcher
// into SessionManager's requestId→sessionId index.
class SmAskUserRouter : public AskUserRouter {
public:
    explicit SmAskUserRouter(SessionManager* sm) : sm_(sm) {}

    void RegisterAskRequest(const std::string& requestId, const std::string& sessionId) override
    {
        std::lock_guard<std::mutex> lock(sm_->askIndexMutex_);
        sm_->askRequestToSession_[requestId] = sessionId;
    }

    void UnregisterAskRequest(const std::string& requestId) override
    {
        std::lock_guard<std::mutex> lock(sm_->askIndexMutex_);
        sm_->askRequestToSession_.erase(requestId);
    }

private:
    SessionManager* sm_;
};

SessionManager& GetSessionManager()
{
    // Lock-free fast path: an acquire-load pairs with the release-store below
    // so callers observe a fully constructed SessionManager. The pointer is
    // published exactly once and never reset, so it never dangles.
    SessionManager* sm = g_sessionManager.load(std::memory_order_acquire);
    if (!sm) {
        std::lock_guard<std::mutex> lock(g_initMutex);
        sm = g_sessionManager.load(std::memory_order_relaxed);
        if (!sm) {
            sm = new SessionManager();
            g_sessionManager.store(sm, std::memory_order_release);
        }
    }
    return *sm;
}

void InitSessionManager(const AgentConfig& config)
{
    // init-once: the singleton is created at most once and never deleted.
    // Runtime reconfiguration goes through ReloadAgent(), not by recreating
    // the SessionManager. Never deleting the instance guarantees the pointer
    // handed out by GetSessionManager() can never become a dangling
    // reference. A repeat call (not expected in practice) simply
    // re-Initialize()s the same instance.
    std::lock_guard<std::mutex> lock(g_initMutex);
    // curl_global_init must run on the main thread before any curl_easy_init
    // and before worker threads start. Idempotent (call_once). Registering it
    // here keeps the curl lifecycle tied to SessionManager bootstrap.
    CurlClient::GlobalInit();
    SessionManager* sm = g_sessionManager.load(std::memory_order_relaxed);
    if (!sm) {
        sm = new SessionManager();
        g_sessionManager.store(sm, std::memory_order_release);
    }
    sm->Initialize(config);
}

namespace {

// Sanitizes a string to be safe for use as a directory/file name on all OS.
// - Replaces Windows reserved chars: \/:*?"<>|
// - Replaces ASCII control characters (0x00-0x1F, 0x7F)
// - Replaces '.' and '..' to prevent path traversal
// - Ensures non-empty result
std::string SanitizePathName(const std::string& name)
{
    if (name.empty())
        return "unnamed";
    if (name == ".")
        return "dot";
    if (name == "..")
        return "dotdot";

    std::string result;
    result.reserve(name.size());
    for (char ch : name) {
        auto c = static_cast<unsigned char>(ch);
        if (c <= 0x1F || c == 0x7F) {
            // Control chars
            result += '_';
        } else if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
                   c == '"' || c == '<' || c == '>' || c == '|') {
            // Windows reserved
            result += '_';
        } else {
            result += static_cast<char>(c);
        }
    }

    // Still empty after sanitize
    if (result.empty())
        return "unnamed";
    return result;
}

} // namespace

SessionManager::SessionManager()
{
    // The core library's own reserved id: MakeSessionKey returns it on
    // (channel, chatId) both-empty, and FindOrCreateEntry pre-creates it
    // during Initialize. Application layers register additional system
    // session ids (cron / heartbeat / etc.) at startup via
    // RegisterReservedSession.
    reservedSessions_.insert(kDefaultSessionId);
}

SessionManager::~SessionManager()
{
    // Destruction order matters: agent_ and the per-session ContextEngine
    // callbacks both hold non-owning raw pointers into memoryRuntime_. They
    // must be torn down before memoryRuntime_ is destroyed. We do that
    // explicitly here (rather than relying solely on reverse member-decl
    // order) so the lifetime contract is enforced by code, not just layout.
    if (agent_) {
        agent_->Shutdown();  // stop the consolidation thread before teardown
    }
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        sessions_.clear();   // drops ContextEngine callbacks capturing memoryRuntime_/historyStore_
    }
    agent_.reset();          // drop the Agent (holds raw MemoryRuntime*/HistoryStore*)
    memoryRuntime_.reset();  // now safe: no surviving non-owning reference
    historyStore_.reset();   // ditto for HistoryStore sink callbacks
}

void SessionManager::Initialize(const AgentConfig& config)
{
    std::lock_guard<std::mutex> lock(sessionMutex_);
    config_ = config;
    initialized_ = false;

    std::string basePath = config_.dataBasePath.empty() ? "./data" : config_.dataBasePath;
    
    // Convert to absolute path to ensure persistence regardless of CWD
    std::string absBasePath;
    try {
        absBasePath = fs::canonical(fs::path(basePath)).string();
    } catch (const std::filesystem::filesystem_error&) {
        // If path doesn't exist yet, use absolute with create_directories approach
        absBasePath = fs::absolute(fs::path(basePath)).string();
    }
    LOG(INFO) << "[SessionManager] Normalizing basePath to absolute: " << absBasePath;
    config_.dataBasePath = absBasePath; // CRITICAL: Update global config
    basePath = absBasePath;
    fs::path rootPath(basePath);
    fs::create_directories(rootPath);
    fs::create_directories(rootPath / "sessions");
    fs::create_directories(rootPath / "memory");

    // Init concurrency gate
    if (config_.maxConcurrentSessions > 0) {
        maxConcurrent_ = config_.maxConcurrentSessions;
    }

    // Build the shared memory runtime before the Agent so its context routing
    // can be wired immediately.
    InitMemoryRuntime();

    // Always create the local fallback HistoryStore (the simplified local
    // MemoryRuntime). Used as the ContextEngine event sink when no
    // MemoryRuntime is configured. Survives reload (created once here).
    historyStore_ = std::make_unique<HistoryStore>(config_.dataBasePath);

    // Create the AskUserRouter and WorkerEnv adapters. These resolve
    // session-scoped resources through SessionManager, eliminating the
    // WorkerEnv→Agent back-reference cycle.
    askRouter_ = std::make_unique<SmAskUserRouter>(this);
    workerEnv_ = std::make_unique<SmWorkerEnv>(this);

    // Create the single shared Agent
    agent_ = std::make_shared<Agent>(config_);
    agent_->SetMemoryRuntime(memoryRuntime_.get());
    agent_->SetHistoryStore(historyStore_.get());
    agent_->SetWorkerEnv(workerEnv_.get());

    // Progressive tool disclosure: register the tool_search escape valve as
    // a session-scoped tool when toolDisclosureMode is PROGRESSIVE or
    // SELECTIVE. DISABLED (and AUTO, which v1 resolves to DISABLED) skip
    // registration so tool_search is absent from FC and the {$tools} prompt
    // (zero regression). Registration lives here (not in
    // ResourceManager::RegisterBuiltinTools) because ResourceManager is a
    // no-config singleton and cannot read toolDisclosureMode; the factory
    // captures only ctx.turnState per invocation plus the full alwaysOn set
    // (meta-tools ∪ config_.alwaysOnTools, computed once at registration
    // time and captured by value) so the load action can idempotently
    // short-circuit on any alwaysOn name (§5.3 注册站点 + §5.3 越界处理:
    // alwaysOn 工具被 load → 幂等返 "already in the FC"). V2: ctx.capabilitySelector
    // is also passed for the search action's real-recall branch (round5
    // §5.4.1 条 8/9: CapabilitySelector reuses main modelConfig for LLM-backed
    // recall). No mode label is threaded into the tool: the search action's
    // short-circuit is driven by TurnState::isActiveFullPool() (runtime
    // active-set state), not a mode value — see tool_search_tool.cpp.
    // ReloadAgent re-registers symmetrically (idempotent overwrite, captures
    // fresh alwaysOn).
    if (config_.toolDisclosureMode == ToolDisclosureMode::PROGRESSIVE
        || config_.toolDisclosureMode == ToolDisclosureMode::SELECTIVE) {
        // Compute the full alwaysOn set once (config-derived static; not
        // per-turn state, so it does not belong on TurnState). Captured
        // by value — the lambda outlives the registration call.
        std::set<std::string> alwaysOnNames = ComputeAlwaysOnFor(config_);
        ResourceManager::GetInstance().RegisterSessionTool(
            "tool_search",
            [alwaysOnNames = std::move(alwaysOnNames)](const ToolBuildContext& ctx) {
                return std::make_unique<ToolSearchTool>(ctx.turnState, alwaysOnNames, ctx.capabilitySelector);
            });
    }

    // Register default tools
    if (!config_.defaultTools.empty()) {
        agent_->AddTools(config_.defaultTools);
    }

    SetupAgentContextRouting();

    // Restore existing sessions from disk
    fs::path sessionsDir = fs::path(basePath) / "sessions";
    std::error_code ec;
    bool dirExists = fs::exists(sessionsDir, ec);
    bool isDir = dirExists ? fs::is_directory(sessionsDir, ec) : false;

    if (dirExists && isDir && !ec) {
        int count = 0;
        try {
            for (const auto& dirEntry : fs::directory_iterator(sessionsDir, ec)) {
                if (ec) break;
                if (dirEntry.is_directory()) {
                    auto dirName = dirEntry.path().filename().string();
                    if (dirName.find('.') != 0 && !dirName.empty()) {
                        try {
                            FindOrCreateEntry(dirName);
                            count++;
                        } catch (const std::exception& e) {
                            LOG(ERR) << "[SessionManager] Failed to restore session '" << dirName << "': " << e.what();
                        }
                    }
                }
            }
        } catch (...) {
            // directory_iterator may throw on Windows even with ec; swallow
        }
        LOG(INFO) << "[SessionManager] Successfully restored " << count << " sessions from disk.";
    } else {
        LOG(INFO) << "[SessionManager] No existing sessions directory found at: " << sessionsDir.string();
    }

    // Ensure __DEFAULT__ session always exists
    try {
        FindOrCreateEntry(kDefaultSessionId);
    } catch (const std::exception& e) {
        LOG(ERR) << "[SessionManager] Failed to create default session: " << e.what();
    }

    initialized_ = true;

    LOG(INFO) << "[SessionManager] Initialization complete. Active sessions: " << sessions_.size();
    for (const auto& pair : sessions_) {
        LOG(INFO) << "  - Session: " << pair.first;
    }
}

std::shared_ptr<ContextEngine> SessionManager::GetContextEngine(const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(sessionMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return nullptr;
    return it->second->contextEngine;
}

void SessionManager::InitMemoryRuntime()
{
    // Caller holds sessionMutex_.
    memoryRuntime_.reset();
    if (!config_.memoryConfig.enabled) {
        LOG(INFO) << "[MemoryRuntime] Disabled (using legacy memory system)";
        return;
    }
    try {
        MemoryConfig memoryConfig = config_.memoryConfig;
        if (memoryConfig.dataPath.empty()) {
            memoryConfig.dataPath = config_.dataBasePath;
        }
        if (memoryConfig.mode == "server") {
            memoryConfig.provider = "http.server";
        }
        memoryRuntime_ = ResourceManager::GetInstance().CreateMemoryRuntime(memoryConfig);
        if (memoryRuntime_) {
            LOG(INFO) << "[MemoryRuntime] Initialized mode=" << memoryConfig.mode
                      << " provider=" << memoryConfig.provider
                      << " dataPath=" << memoryConfig.dataPath;
        } else {
            LOG(WARN) << "[MemoryRuntime] Initialization returned null, falling back to legacy memory";
        }
    } catch (const std::exception& e) {
        LOG(WARN) << "[MemoryRuntime] Initialization failed: " << e.what()
                  << ", falling back to legacy memory system";
        memoryRuntime_.reset();
    }
}

void SessionManager::SetupAgentContextRouting()
{
    // Use workerEnv_ to resolve ContextEngine by sessionId. During an
    // Invoke, SmWorkerEnv has the current entry cached (set via
    // SetCurrentEntry before invokeMutex acquisition), so no
    // sessionMutex_ acquisition is needed — eliminating the
    // invokeMutex→sessionMutex_ lock-ordering violation. Outside
    // Invoke, the fallback path acquires sessionMutex_.
    agent_->SetContextEngineGetter(
        [this](const std::string& sessionId) -> std::shared_ptr<ContextEngine> {
            return workerEnv_->GetContextEngine(sessionId);
        }
    );
}

std::shared_ptr<SessionEntry> SessionManager::FindOrCreateEntry(const std::string& sessionId)
{
    // Caller must hold sessionMutex_
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        return it->second;
    }

    auto entry = std::make_shared<SessionEntry>();
    entry->sessionId = sessionId;

    // 绑定此刻的活跃 Agent。调用方（Initialize / Invoke / GetOrCreateSession）
    // 均持 sessionMutex_，agent_ 在该锁下被 ReloadAgent swap，故绑定读到的是
    // 一致的活跃 Agent。ReloadAgent 后旧 Agent 由本引用保活直至该 session
    // 在途回合跑完。
    entry->agent = agent_;

    entry->todoList = std::make_unique<SessionTodoList>();
    entry->askUser = std::make_unique<AskUserDispatcher>(sessionId, askRouter_.get());

    // Build per-session ContextEngine
    ContextConfig ctxConfig = config_.contextConfig;
    ctxConfig.sessionId = sessionId;

    std::string basePath = config_.dataBasePath.empty() ? "./data" : config_.dataBasePath;
    fs::path sessionDir = fs::path(basePath) / "sessions" / SanitizePathName(sessionId);
    fs::create_directories(sessionDir / "context");
    ctxConfig.storagePath = (sessionDir / "context").string();

    entry->contextEngine = std::make_shared<ContextEngine>(ctxConfig);
    entry->contextEngine->Initialize();

    MemoryRuntime* memoryRuntime = memoryRuntime_.get();
    if (memoryRuntime) {
        entry->contextEngine->SetMemoryContextProvider([memoryRuntime, sessionId, agentId = config_.id]() {
            MemoryContextRequest request;
            request.agentId = agentId;
            request.sessionId = sessionId;
            LOG(INFO) << "[MemoryRuntime] BuildContext begin agentId=" << agentId << " sessionId=" << sessionId;
            MemoryContextPackage context = memoryRuntime->BuildContext(request);
            LOG(INFO) << "[MemoryRuntime] BuildContext end agentId=" << agentId << " sessionId=" << sessionId
                      << " memoryTextChars=" << context.memoryText.size()
                      << " payloadRefs=" << context.payloadRefs.size();
            if (!context.memoryText.empty()) {
                LOG(INFO) << "[MemoryRuntime] BuildContext memoryText begin\n"
                          << context.memoryText
                          << "\n[MemoryRuntime] BuildContext memoryText end";
            } else {
                LOG(INFO) << "[MemoryRuntime] BuildContext memoryText empty";
            }
            return context.memoryText;
        });
        entry->contextEngine->SetMemoryEventSink([memoryRuntime, agentId = config_.id](const MemoryEvent& event) {
            MemoryEvent copied = event;
            copied.agentId = agentId;
            if (!memoryRuntime->AppendEvent(copied)) {
                LOG(WARN) << "[SessionManager] Memory AppendEvent lost: agentId=" << agentId
                          << " sessionId=" << copied.sessionId << " eventType=" << static_cast<int>(copied.type);
            }
        });
    } else if (historyStore_) {
        // No MemoryRuntime configured (or its init failed): route the event
        // stream to the local fallback HistoryStore (simplified local
        // MemoryRuntime). Content is aligned with MemoryRuntime (full
        // MemoryEvent fields), so DreamProcessor mines the same shape of data.
        HistoryStore* hs = historyStore_.get();
        entry->contextEngine->SetMemoryEventSink([hs](const MemoryEvent& event) {
            hs->AppendEvent(event);
        });
    }

    sessions_[sessionId] = std::move(entry);
    return sessions_[sessionId];
}

SessionInvokeResult SessionManager::Invoke(
    const std::string& sessionId,
    const std::string& message,
    std::function<void(const std::string&)> callback)
{
    LOG(INFO) << "[SessionManager] [" << sessionId << "] User query begin, chars=" << message.length()
              << "\n" << message
              << "\n[SessionManager] [" << sessionId << "] User query end";

    if (!initialized_) {
        return SessionInvokeResult{sessionId, false, "Not initialized", "[ERROR] SessionManager not initialized"};
    }

    // Optional concurrency cap (maxConcurrentSessions). ReloadAgent no longer
    // raises a reload barrier (graceful shutdown: in-flight calls keep running
    // on the draining old Agent), so this gate is purely a limiter.
    {
        std::unique_lock<std::mutex> lock(concurrencyMutex_);
        if (maxConcurrent_ > 0) {
            concurrencyCv_.wait(lock, [this](){
                return concurrentCount_ < maxConcurrent_;
            });
        }
        ++concurrentCount_;
    }

    auto releaseGate = [this]() {
        std::lock_guard<std::mutex> lock(concurrencyMutex_);
        if (concurrentCount_ > 0) --concurrentCount_;
        concurrencyCv_.notify_all();
    };

    // Snapshot the Agent bound to this session. New sessions bind the active
    // agent_ at creation (FindOrCreateEntry); ReloadAgent swaps agent_ under
    // sessionMutex_ and marks the old Agent draining. If this session's
    // bound Agent is draining (a prior reload retired it), rebind to the
    // current active Agent so this NEW turn runs on the new Agent. All of
    // this happens under sessionMutex_ (same lock as the swap), so the
    // read/rebind is race-free with ReloadAgent. The shared_ptr copy also
    // guarantees the Agent stays alive for the full turn even if another
    // reload retires it mid-turn.
    std::shared_ptr<Agent> agentPtr;

    // Find or create session entry
    std::shared_ptr<SessionEntry> entry;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        entry = FindOrCreateEntry(sessionId);
        if (entry->agent && entry->agent->IsDraining()) {
            entry->agent = agent_;  // rebind to current active Agent
        }
        agentPtr = entry->agent;
    }

    if (!entry || !entry->contextEngine || !agentPtr) {
        releaseGate();
        return SessionInvokeResult{sessionId, false, "Create failed", "[ERROR] Failed to create session"};
    }

    // Cache the session entry in SmWorkerEnv so that subsequent
    // ContextEngine/todoList/askUser lookups during Invoke can access
    // them directly without acquiring sessionMutex_ (eliminating the
    // invokeMutex→sessionMutex_ lock-ordering violation per the
    // lock ordering protocol).
    workerEnv_->SetCurrentEntry(entry);

    // Per-session lock (serializes calls within same session)
    std::unique_lock<std::mutex> lock(entry->invokeMutex);
    entry->isBusy = true;

    std::string result;
    try {
        result = agentPtr->Invoke(sessionId, message, callback);
    } catch (const std::exception& e) {
        entry->isBusy = false;
        workerEnv_->ClearCurrentEntry();
        releaseGate();

        std::string err = "Invoke failed: " + std::string(e.what());
        LOG(ERR) << "[SessionManager] [" << sessionId << "] " << err;

        return SessionInvokeResult{sessionId, false, e.what(), ""};
    }

    entry->isBusy = false;
    workerEnv_->ClearCurrentEntry();
    releaseGate();

    return SessionInvokeResult{sessionId, true, "", result};
}

SessionInvokeResult SessionManager::InvokeChannel(
    const ChannelMessage& msg,
    std::function<void(const std::string&)> callback)
{
    std::string sessionId = msg.sessionId;
    if (sessionId.empty()) {
        sessionId = MakeSessionKey(msg.channel, msg.chatId);
    }
    return Invoke(sessionId, msg.content, callback);
}

void SessionManager::Cancel()
{
    // Cancels the ACTIVE Agent's in-flight worker only. Draining (retired)
    // Agents are intentionally not cancelled: their in-flight calls belong
    // to existing sessions and the operator "abort current generation"
    // semantics should not cross Agent boundaries. Per-session / per-Agent
    // cancel is out of scope (full AgentPool territory).
    // The agent_ read is guarded by sessionMutex_ (the same lock ReloadAgent
    // swaps agent_ under) so the shared_ptr read cannot race with a reload.
    // Agent::Cancel just bumps an atomic generation counter, so the lock hold
    // is brief and acquires no nested locks.
    std::shared_ptr<Agent> active;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        active = agent_;
    }
    if (active) {
        active->Cancel();
    }
}

void SessionManager::Shutdown()
{
    // Stop background consolidation threads so they are joined here rather
    // than at static teardown. With graceful shutdown there may be multiple
    // live Agents at once (the active one plus any draining ones still
    // servicing in-flight turns on sessions bound to them). We collect the
    // deduped set of all surviving Agent shared_ptrs (active + every
    // session's bound agent) under sessionMutex_, then Shutdown each one
    // outside the lock. Agent::Shutdown is idempotent, so the dedup is only
    // to reduce noise. This guarantees no consolidation thread is left
    // running when the process exits (which would risk data loss / UB at
    // static destruction). Note: in-flight Invokes holding their own
    // shared_ptr keep the Agent alive past this point; that is expected —
    // Shutdown joins the consolidation thread, it does NOT abort in-flight
    // turns. Callers must still stop all curl-using threads (heartbeat,
    // cron, channels, HTTP) BEFORE invoking Shutdown.
    std::vector<std::shared_ptr<Agent>> agents;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (agent_) {
            agents.push_back(agent_);
        }
        for (const auto& p : sessions_) {
            if (p.second && p.second->agent) {
                agents.push_back(p.second->agent);
            }
        }
    }
    // Dedupe (same Agent may be bound to multiple sessions).
    std::sort(agents.begin(), agents.end(),
              [](const std::shared_ptr<Agent>& a, const std::shared_ptr<Agent>& b) {
                  return a.get() < b.get();
              });
    agents.erase(std::unique(agents.begin(), agents.end(),
                             [](const std::shared_ptr<Agent>& a, const std::shared_ptr<Agent>& b) {
                                 return a.get() == b.get();
                             }),
                 agents.end());

    for (auto& a : agents) {
        a->Shutdown();
    }
    // GlobalCleanup contract: curl_global_cleanup must run AFTER all
    // thread_local CURL handles are destroyed (i.e. after all curl-using
    // threads exit). SessionManager only owns the Agent consolidation
    // threads (joined above); the caller MUST stop all other curl-using
    // threads (heartbeat, cron, channels, HTTP) BEFORE invoking Shutdown —
    // see main.cpp's stop order (ConfigWatcher/Channels/HttpServer/cron/
    // heartbeat all stopped before Shutdown). GlobalCleanup is idempotent
    // (call_once). The atexit fallback registered in GlobalInit covers the
    // "Shutdown never called" case (runs at process exit, after all
    // thread_local destruction); it does NOT defend against the contract
    // violation of calling Shutdown while curl threads are still running —
    // that case needs explicit thread joining by the caller.
    CurlClient::GlobalCleanup();
}

bool SessionManager::IsSessionBusy(const std::string& sessionId) const
{
    // Query the Agent the session is BOUND to (entry->agent), not the global
    // active agent_. A session bound to a draining old Agent is tracked in
    // THAT Agent's sessionActivity_; querying the active Agent would yield a
    // false negative. Capture entry->agent under sessionMutex_ (same lock
    // ReloadAgent swaps agent_ under, so the shared_ptr read cannot race),
    // then release before calling Agent::IsSessionBusy (which acquires
    // sessionActivityMutex_ L4) to respect the L2→L4 lock ordering.
    std::shared_ptr<Agent> bound;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end() || !it->second) return false;
        bound = it->second->agent;
    }
    if (bound) return bound->IsSessionBusy(sessionId);
    return false;
}

void SessionManager::RegisterReservedSession(std::string id)
{
    if (id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(reservedMutex_);
    reservedSessions_.insert(std::move(id));
}

void SessionManager::RemoveSession(const std::string& sessionId)
{
    if (sessionId.empty()) {
        LOG(WARN) << "[SessionManager] Cannot delete empty session.";
        return;
    }

    // Reserved-status check is intentionally a separate short critical
    // section from the sessionMutex_ find/erase below. Holding
    // reservedMutex_ across the disk deletion inside sessionMutex_
    // would block RegisterReservedSession for the duration of
    // fs::remove_all, which can be slow. The trade-off is a TOCTOU
    // window: if another thread calls RegisterReservedSession(sameId)
    // between this check and the erase, the deletion still proceeds.
    //
    // Accepted risk: reservedSessions_ is populated only at application
    // startup (jiuwenClaw registers __HEARTBEAT__/__CRON__ in main.cpp
    // before any HTTP server starts accepting requests). Runtime
    // registration is effectively non-existent, so the window is
    // practically unreachable. Worst case is a just-registered session
    // getting deleted (data protection failure, not a crash). Re-evaluate
    // if a future feature starts dynamically registering reserved
    // sessions at runtime.
    bool isReserved = false;
    {
        std::lock_guard<std::mutex> lock(reservedMutex_);
        isReserved = reservedSessions_.count(sessionId) > 0;
    }
    if (isReserved) {
        LOG(WARN) << "[SessionManager] Cannot delete reserved session: " << sessionId;
        return;
    }

    std::string basePath = config_.dataBasePath.empty() ? "./data" : config_.dataBasePath;
    fs::path sessionDir = fs::path(basePath) / "sessions" / SanitizePathName(sessionId);

    // Single critical section: find -> read isBusy -> erase, all under one
    // lock_guard (no manual unlock, no TOCTOU). The entry is moved out into a
    // local shared_ptr so that an in-flight Invoke holding its own copy keeps
    // the SessionEntry (and its invokeMutex) alive; the real destruction is
    // deferred until the last reference is released.
    std::shared_ptr<SessionEntry> removed;
    bool wasInMemory = false;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        auto it = sessions_.find(sessionId);
        if (it != sessions_.end()) {
            wasInMemory = true;
            if (it->second->isBusy.load(std::memory_order_acquire)) {
                LOG(WARN) << "[SessionManager] Removing busy session (soft delete): " << sessionId;
            }
            removed = std::move(it->second);
            sessions_.erase(it);
            LOG(INFO) << "[SessionManager] In-memory session removed: " << sessionId;
        }
        // else: not in memory, but may still exist on disk; fall through.
    }

    // Disk deletion. Guard against deleting files out from under an in-flight
    // Invoke that is still writing this session's ContextEngine storage. A
    // non-blocking try_lock on invokeMutex succeeds only when no Invoke holds
    // it; otherwise we skip the disk removal (the directory is left for a
    // later cleanup) rather than racing remove_all against concurrent writes.
    bool skipDisk = false;
    if (removed) {
        if (removed->invokeMutex.try_lock()) {
            // No in-flight Invoke: safe to delete the directory.
            removed->invokeMutex.unlock();
        } else {
            skipDisk = true;
            LOG(WARN) << "[SessionManager] Session busy; skipped disk deletion: " << sessionId;
        }
    }
    // If it was never in memory (removed == nullptr), there can be no in-flight
    // Invoke for it, so disk removal is safe.
    (void)wasInMemory;

    if (!skipDisk && fs::exists(sessionDir)) {
        try {
            fs::remove_all(sessionDir);
            LOG(INFO) << "[SessionManager] Session directory deleted: " << sessionDir.string();
        } catch (const std::exception& e) {
            LOG(ERR) << "[SessionManager] Failed to delete session directory: " << e.what();
        }
    }
    // 'removed' is destroyed here; if an in-flight Invoke still references the
    // SessionEntry, actual destruction is deferred until that reference drops.

    // Clean up Agent::sessionActivity_ for the removed session. Called outside
    // sessionMutex_ to avoid lock-ordering violation (session > activity).
    // Use the Agent the session was BOUND to (removed->agent), not the global
    // active agent_: a session bound to a draining old Agent is tracked in
    // THAT Agent's sessionActivity_; cleaning the active Agent would be a
    // no-op and leave a stale entry in the old Agent's map. removed->agent is
    // safe to read here: the entry is no longer in sessions_ (so no Invoke can
    // rebind it) and ReloadAgent never mutates entry->agent, so there is no
    // concurrent writer for THIS entry's agent field.
    std::shared_ptr<Agent> boundAgent = removed ? removed->agent : nullptr;
    // If the session was never in memory, fall back to the active agent so a
    // stale activity entry (if any) is still cleaned.
    if (!boundAgent) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        boundAgent = agent_;
    }
    if (boundAgent) {
        boundAgent->CleanupSession(sessionId);
    }
}

std::shared_ptr<ContextEngine> SessionManager::GetOrCreateSession(
    const std::string& sessionId,
    const SessionConfig& sessionConfig)
{
    std::lock_guard<std::mutex> lock(sessionMutex_);
    auto entry = FindOrCreateEntry(sessionId);
    (void)sessionConfig;
    return entry->contextEngine;
}

std::vector<std::string> SessionManager::GetSessionIds() const
{
    std::lock_guard<std::mutex> lock(sessionMutex_);
    std::vector<std::string> ids;
    ids.reserve(sessions_.size());
    for (const auto& p : sessions_) {
        ids.push_back(p.first);
    }
    return ids;
}

std::vector<Message> SessionManager::GetSessionMessages(const std::string& sessionId) const
{
    // Release sessionMutex_ (L2) before calling GetAllMessages (which
    // acquires ContextEngine::memoryMutex_ at L6) to avoid lock-ordering
    // violation: L2 locks must be released before L6.
    std::shared_ptr<ContextEngine> ce;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end() || !it->second || !it->second->contextEngine) {
            return {};
        }
        ce = it->second->contextEngine;
    }
    return ce->GetAllMessages();
}

std::map<std::string, std::string> SessionManager::GetSessionMetadata(const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(sessionMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return {};
    return it->second->metadata;
}

std::string SessionManager::MakeSessionKey(const std::string& channel, const std::string& chatId)
{
    if (channel.empty() && chatId.empty()) {
        return kDefaultSessionId;
    }
    return channel + "_" + chatId;
}

bool SessionManager::ProvideUserResponse(const std::string& requestId, const std::string& answer)
{
    // Step 1: look up which session this requestId belongs to.
    std::string sessionId;
    {
        std::lock_guard<std::mutex> lock(askIndexMutex_);
        auto it = askRequestToSession_.find(requestId);
        if (it == askRequestToSession_.end()) {
            return false;
        }
        sessionId = it->second;
    }

    // Step 2: find the session entry and forward to its dispatcher.
    std::shared_ptr<SessionEntry> entry;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end() || !it->second) {
            return false;
        }
        entry = it->second;
    }

    if (!entry->askUser) {
        return false;
    }
    return entry->askUser->ProvideResponse(requestId, answer);
}

bool SessionManager::ReloadAgent(const AgentConfig& newConfig, std::string* errorOut)
{
    if (!initialized_) {
        LOG(WARN) << "[SessionManager] ReloadAgent called before Initialize; ignoring";
        if (errorOut) *errorOut = "SessionManager not initialized";
        return false;
    }

    LOG(INFO) << "[SessionManager] ReloadAgent: graceful swap (no drain, no cancel)...";

    // 1. Construct the new Agent BEFORE touching the old one; if construction
    //    fails we keep the old Agent untouched (no drain barrier to lower).
    std::shared_ptr<Agent> newAgent;
    try {
        AgentConfig effective = newConfig;
        // Preserve normalized basePath (Initialize made it absolute).
        if (effective.dataBasePath.empty()) {
            effective.dataBasePath = config_.dataBasePath;
        }
        newAgent = std::make_shared<Agent>(effective);
        // Reuse the existing shared memory runtime so the ContextEngine
        // callbacks captured for live sessions stay valid across the swap.
        newAgent->SetMemoryRuntime(memoryRuntime_.get());
        // Reuse the shared HistoryStore (survives reload, like memoryRuntime_).
        newAgent->SetHistoryStore(historyStore_.get());
        // Inject the SessionManager-owned WorkerEnv so the worker can resolve
        // session-scoped resources without a back-reference to Agent.
        newAgent->SetWorkerEnv(workerEnv_.get());
        // Progressive disclosure: re-register tool_search if the reloaded
        // config enables it (PROGRESSIVE/SELECTIVE). RegisterSessionTool is
        // an idempotent overwrite (operator[] + cache erase), so re-registering
        // on each reload is safe. If the new config is DISABLED/AUTO, the
        // branch is skipped and any previous progressive registration lingers
        // in the shared RM — this is DELIBERATE, not a leak to fix:
        //   - Graceful drain: a draining (old) progressive Agent's FC still
        //     contains tool_search (alwaysOn hardcodes it), so an in-flight
        //     turn may legitimately call it; unregistering from the shared
        //     RM would break that call and violate "in-flight turns must run
        //     to completion". The stale factory keeps draining Agents working.
        //   - New disabled Agent is unaffected: its toolNames_ does not include
        //     tool_search (defaultTools comes from config, not re-fetched from
        //     RM), so BuildToolSchemas/BuildPrompt never emit it → the model
        //     cannot legitimately call it under native FC.
        //   - Residual reachability is only via the direct ExecuteTool path
        //     under prompt-mode hallucination (§8 invariant 3 soft constraint,
        //     §10 TODO) or an explicit tool_search entry in defaultTools (user
        //     misconfig). If reached:
        //       * load writes to loadedTools (dead state — disabled
        //         BuildToolSchemas reads toolNames_, not loadedTools).
        //       * search branches on isActiveFullPool(). The flag's value
        //         depends on session history, not just the current disabled
        //         mode: a FRESH session (proxy never seeded) has flag=false
        //         → real-recall branch → v1 stub returns empty → "No tools
        //         found"; but a session that ran progressive turns BEFORE
        //         this PROGRESSIVE→DISABLED reload has a stale flag=true
        //         (disabled's Invoke skips the IsProgressiveDisclosureActive()
        //         reset+seed block, and reset() is the only flag-clearing
        //         path), so search short-circuits → "No recall needed...".
        //         Both responses are inert (no state mutation, no correctness
        //         impact); the difference is only which inert message shows.
        // A DISABLED→PROGRESSIVE transition re-registers below, overwriting.
        if (effective.toolDisclosureMode == ToolDisclosureMode::PROGRESSIVE
            || effective.toolDisclosureMode == ToolDisclosureMode::SELECTIVE) {
            // Re-compute alwaysOn from the effective (post-reload) config so
            // a changed alwaysOnTools takes effect immediately (mirrors the
            // Initialize-time registration above).
            std::set<std::string> alwaysOnNames = ComputeAlwaysOnFor(effective);
            ResourceManager::GetInstance().RegisterSessionTool(
                "tool_search",
                [alwaysOnNames = std::move(alwaysOnNames)](const ToolBuildContext& ctx) {
                    return std::make_unique<ToolSearchTool>(ctx.turnState, alwaysOnNames, ctx.capabilitySelector);
                });
        }
        if (!effective.defaultTools.empty()) {
            newAgent->AddTools(effective.defaultTools);
        }
        // Wire up MCP tools that were loaded into ResourceManager: without
        // this, a hot-reloaded Agent starts with an empty MCP tool set even
        // though the mcp_servers.json connections are live.
        int mcpDelta = newAgent->SyncMcpTools();
        if (mcpDelta != 0) {
            LOG(INFO) << "[SessionManager] ReloadAgent: synced " << mcpDelta
                      << " MCP tool(s) into reloaded agent";
        }
    } catch (const std::exception& e) {
        LOG(ERR) << "[SessionManager] ReloadAgent: new Agent construction failed: "
                 << e.what() << ". Keeping old Agent.";
        if (errorOut) *errorOut = e.what();
        return false;  // old Agent untouched
    }

    // 2. Atomic swap under sessionMutex_ (same lock Invoke uses for the
    //    entry->agent read/rebind): active Agent becomes the new Agent; the
    //    old Agent is marked draining — it no longer accepts NEW turns from
    //    sessions bound to it (Invoke rebinds draining-bound sessions to the
    //    active Agent), but it continues servicing in-flight turns already
    //    running on it (those calls hold a shared_ptr to it). The old Agent
    //    is NOT cancelled: in-flight turns must run to completion.
    std::shared_ptr<Agent> oldAgent;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        config_ = newConfig;
        if (config_.maxConcurrentSessions > 0) {
            maxConcurrent_ = config_.maxConcurrentSessions;
        } else {
            maxConcurrent_ = 0;
        }
        oldAgent = std::move(agent_);
        agent_ = std::move(newAgent);
        if (oldAgent) {
            oldAgent->MarkDraining();  // no new turns; in-flight ones continue
        }
        SetupAgentContextRouting();
    }

    // 3. The old Agent is NOT cancelled and NOT explicitly released here.
    //    Its shared_ptr is kept alive by every SessionEntry that bound it
    //    (entry->agent). As each bound session's in-flight turn completes
    //    and the entry is reused for a new turn, Invoke rebinds entry->agent
    //    to the active Agent, dropping the old reference. When the last
    //    reference drops, ~Agent -> Shutdown joins the consolidation thread
    //    and the old Agent is reclaimed naturally. 'oldAgent' here only
    //    releases ReloadAgent's own local reference.
    LOG(INFO) << "[SessionManager] ReloadAgent: graceful swap complete. Old agent draining, "
              << "sessions preserved=" << GetSessionIds().size();
    return true;
}

} // namespace jiuwen

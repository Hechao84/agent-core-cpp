#include "include/session_manager.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include "include/agent.h"
#include "include/resource_manager.h"
#include "src/context_engine/context_engine.h"
#include "src/core/ask_user_dispatcher.h"
#include "src/core/session_todo_list.h"
#include "src/core/worker_env.h"
#include "src/utils/data_dir.h"
#include "src/utils/logger.h"

namespace fs = std::filesystem;

namespace jiuwen {

namespace {
    std::atomic<SessionManager*> g_sessionManager{nullptr};
    std::mutex g_initMutex;
}

// WorkerEnv adapter that resolves session-scoped resources through
// SessionManager. This eliminates the AgentWorker → WorkerEnv → Agent
// back-reference cycle: the worker only depends on the
// WorkerEnv interface, and this implementation routes via SessionManager
// which owns SessionEntry (todoList, askUser) and memoryRuntime.
class SmWorkerEnv : public WorkerEnv {
public:
    explicit SmWorkerEnv(SessionManager* sm) : sm_(sm) {}

    SessionTodoList* GetOrCreateSessionTodoList(const std::string& sessionId) override
    {
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

private:
    SessionManager* sm_;
};

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

SessionManager::SessionManager() = default;

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
        sessions_.clear();   // drops ContextEngine callbacks capturing memoryRuntime_
    }
    agent_.reset();          // drop the Agent (holds a raw MemoryRuntime*)
    memoryRuntime_.reset();  // now safe: no surviving non-owning reference
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

    // Create the AskUserRouter and WorkerEnv adapters. These resolve
    // session-scoped resources through SessionManager, eliminating the
    // WorkerEnv→Agent back-reference cycle.
    askRouter_ = std::make_unique<SmAskUserRouter>(this);
    workerEnv_ = std::make_unique<SmWorkerEnv>(this);

    // Create the single shared Agent
    agent_ = std::make_shared<Agent>(config_);
    agent_->SetMemoryRuntime(memoryRuntime_.get());
    agent_->SetWorkerEnv(workerEnv_.get());

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
    // Use a raw pointer to "this" since SessionManager outlives Agent
    SessionManager* self = this;
    agent_->SetContextEngineGetter(
        [self](const std::string& sessionId) -> std::shared_ptr<ContextEngine> {
            return self->GetContextEngine(sessionId);
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
            memoryRuntime->AppendEvent(copied);
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
        return SessionInvokeResult{"[ERROR] SessionManager not initialized", false, "Not initialized", sessionId};
    }

    // Wait if a reload is in progress, then mark ourselves as in-flight.
    // 'concurrentCount_' is reused as the reload-drain counter; it always
    // tracks the number of Invoke calls currently between the gate enter
    // and gate exit, regardless of whether the optional maxConcurrent gate
    // is active.
    {
        std::unique_lock<std::mutex> lock(concurrencyMutex_);
        reloadCv_.wait(lock, [this](){ return !reloading_; });
        // If the optional concurrency cap is enabled, also wait for a slot.
        if (maxConcurrent_ > 0) {
            concurrencyCv_.wait(lock, [this](){
                return !reloading_ && concurrentCount_ < maxConcurrent_;
            });
        }
        ++concurrentCount_;
    }

    auto releaseGate = [this]() {
        std::lock_guard<std::mutex> lock(concurrencyMutex_);
        if (concurrentCount_ > 0) --concurrentCount_;
        concurrencyCv_.notify_all();
        reloadCv_.notify_all();
    };

    // Snapshot the current Agent; if a reload swaps after this point we
    // still safely use the prior Agent for this call. The shared_ptr copy
    // also guarantees the Agent stays alive even though the drain in
    // ReloadAgent already waits for releaseGate before destruction.
    std::shared_ptr<Agent> agentPtr;

    // Find or create session entry
    std::shared_ptr<SessionEntry> entry;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        entry = FindOrCreateEntry(sessionId);
        agentPtr = agent_;
    }

    if (!entry || !entry->contextEngine || !agentPtr) {
        releaseGate();
        return SessionInvokeResult{"[ERROR] Failed to create session", false, "Create failed", sessionId};
    }

    // Per-session lock (serializes calls within same session)
    std::unique_lock<std::mutex> lock(entry->invokeMutex);
    entry->isBusy = true;

    std::string result;
    try {
        result = agentPtr->Invoke(sessionId, message, callback);
    } catch (const std::exception& e) {
        entry->isBusy = false;
        releaseGate();

        std::string err = "Invoke failed: " + std::string(e.what());
        LOG(ERR) << "[SessionManager] [" << sessionId << "] " << err;

        return SessionInvokeResult{"", false, e.what(), sessionId};
    }

    entry->isBusy = false;
    releaseGate();

    return SessionInvokeResult{result, true, "", sessionId};
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
    if (agent_) {
        agent_->Cancel();
    }
}

void SessionManager::Shutdown()
{
    // Stop the Agent's background consolidation thread so it is joined here
    // rather than at static teardown. The singleton itself is never deleted
    // (see InitSessionManager), so we only drain the Agent. Idempotent:
    // Agent::Shutdown is safe to call repeatedly.
    std::shared_ptr<Agent> agent = agent_;
    if (agent) {
        agent->Shutdown();
    }
}

bool SessionManager::IsSessionBusy(const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(sessionMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return false;

    // Use agent's activity tracking
    auto amu = agent_.get();
    if (amu) return amu->IsSessionBusy(sessionId);
    return false;
}

void SessionManager::RemoveSession(const std::string& sessionId)
{
    if (sessionId.empty() ||
        sessionId == kDefaultSessionId ||
        sessionId == kHeartbeatSessionId ||
        sessionId == kCronSessionId) {
        LOG(WARN) << "[SessionManager] Cannot delete reserved or empty session.";
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
    std::lock_guard<std::mutex> lock(sessionMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end() || !it->second || !it->second->contextEngine) {
        return {};
    }
    return it->second->contextEngine->GetAllMessages();
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

    LOG(INFO) << "[SessionManager] ReloadAgent: draining in-flight requests...";

    // 1. Raise the reload barrier so new Invokes wait.
    {
        std::lock_guard<std::mutex> lock(concurrencyMutex_);
        reloading_ = true;
    }

    // 2. Wait until all in-flight Invoke calls have completed.
    {
        std::unique_lock<std::mutex> lock(concurrencyMutex_);
        concurrencyCv_.wait(lock, [this](){ return concurrentCount_ == 0; });
    }

    // 3. Build the new Agent BEFORE swapping; if construction fails,
    //    we keep the old one and lower the barrier.
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
        // Inject the SessionManager-owned WorkerEnv so the worker can resolve
        // session-scoped resources without a back-reference to Agent.
        newAgent->SetWorkerEnv(workerEnv_.get());
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
        {
            std::lock_guard<std::mutex> lock(concurrencyMutex_);
            reloading_ = false;
        }
        reloadCv_.notify_all();
        LOG(ERR) << "[SessionManager] ReloadAgent: new Agent construction failed: "
                 << e.what() << ". Keeping old Agent.";
        if (errorOut) *errorOut = e.what();
        return false;
    }

    // 4. Atomic swap (under sessionMutex_ to serialize with new Invokes).
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
        SetupAgentContextRouting();
    }

    // 5. Lower the barrier — let waiting Invokes proceed.
    {
        std::lock_guard<std::mutex> lock(concurrencyMutex_);
        reloading_ = false;
    }
    reloadCv_.notify_all();
    concurrencyCv_.notify_all();

    // 6. Cancel and release the old agent OUTSIDE the lock.
    if (oldAgent) {
        oldAgent->Cancel();
        oldAgent.reset();
    }

    LOG(INFO) << "[SessionManager] ReloadAgent: swap complete. Sessions preserved="
              << GetSessionIds().size();
    return true;
}

} // namespace jiuwen

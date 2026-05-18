#include "include/session_manager.h"
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include "include/agent.h"
#include "src/context_engine/context_engine.h"
#include "src/utils/data_dir.h"
#include "src/utils/logger.h"

namespace fs = std::filesystem;

namespace jiuwen {

namespace {
    SessionManager* g_sessionManager = nullptr;
    std::mutex g_initMutex;
}

SessionManager& GetSessionManager()
{
    if (!g_sessionManager) {
        std::lock_guard<std::mutex> lock(g_initMutex);
        if (!g_sessionManager) {
            g_sessionManager = new SessionManager();
        }
    }
    return *g_sessionManager;
}

void InitSessionManager(const AgentConfig& config)
{
    std::lock_guard<std::mutex> lock(g_initMutex);
    if (g_sessionManager) {
        delete g_sessionManager;
        g_sessionManager = nullptr;
    }
    g_sessionManager = new SessionManager();
    g_sessionManager->Initialize(config);
}

SessionManager::SessionManager() = default;

SessionManager::~SessionManager()
{
    if (agent_) {
        agent_->Cancel();
    }
    std::lock_guard<std::mutex> lock(sessionMutex_);
    sessions_.clear();
}

void SessionManager::Initialize(const AgentConfig& config)
{
    std::lock_guard<std::mutex> lock(sessionMutex_);
    config_ = config;
    initialized_ = true;

    std::string basePath = config_.dataBasePath.empty() ? "./data" : config_.dataBasePath;
    fs::create_directories(fs::path(basePath));
    fs::create_directories(fs::path(basePath) / "sessions");
    fs::create_directories(fs::path(basePath) / "memory");

    // Init concurrency gate
    if (config_.maxConcurrentSessions > 0) {
        maxConcurrent_ = config_.maxConcurrentSessions;
    }

    // Create the single shared Agent
    agent_ = std::make_unique<Agent>(config_);

    // Register default tools on the shared Agent
    if (!config_.defaultTools.empty()) {
        agent_->AddTools(config_.defaultTools);
    }

    // Set up context engine routing so Agent can find per-session ContextEngine
    SetupAgentContextRouting();

    LOG(INFO) << "[SessionManager] Single-Agent mode initialized, basePath=" << basePath
              << ", maxConcurrent=" << maxConcurrent_;
}

std::shared_ptr<ContextEngine> SessionManager::GetContextEngine(const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(sessionMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return nullptr;
    return it->second->contextEngine;
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

SessionEntry* SessionManager::FindOrCreateEntry(const std::string& sessionId)
{
    // Caller must hold sessionMutex_
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        return it->second.get();
    }

    auto entry = std::make_unique<SessionEntry>();
    entry->sessionId = sessionId;

    // Build per-session ContextEngine
    ContextConfig ctxConfig = config_.contextConfig;
    ctxConfig.sessionId = sessionId;

    std::string basePath = config_.dataBasePath.empty() ? "./data" : config_.dataBasePath;
    fs::path sessionDir = fs::path(basePath) / "sessions" / sessionId;
    fs::create_directories(sessionDir / "context");
    ctxConfig.storagePath = (sessionDir / "context").string();

    entry->contextEngine = std::make_shared<ContextEngine>(ctxConfig);
    entry->contextEngine->Initialize();

    sessions_[sessionId] = std::move(entry);
    return sessions_[sessionId].get();
}

SessionInvokeResult SessionManager::Invoke(
    const std::string& sessionId,
    const std::string& message,
    std::function<void(const std::string&)> callback)
{
    if (!initialized_) {
        return SessionInvokeResult{"[ERROR] SessionManager not initialized", false, "Not initialized", sessionId};
    }

    // Find or create session entry
    SessionEntry* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        entry = FindOrCreateEntry(sessionId);
    }

    if (!entry || !entry->contextEngine) {
        return SessionInvokeResult{"[ERROR] Failed to create session", false, "Create failed", sessionId};
    }

    // Per-session lock (serializes calls within same session)
    std::unique_lock<std::mutex> lock(entry->invokeMutex);
    entry->isBusy = true;

    // Global concurrency gate
    if (maxConcurrent_ > 0) {
        AcquireConcurrency();
    }

    std::string result;
    try {
        result = agent_->Invoke(sessionId, message, callback);
    } catch (const std::exception& e) {
        entry->isBusy = false;

        if (maxConcurrent_ > 0) {
            ReleaseConcurrency();
        }

        std::string err = "Invoke failed: " + std::string(e.what());
        LOG(ERR) << "[SessionManager] " << err;

        return SessionInvokeResult{"", false, e.what(), sessionId};
    }

    // Release concurrency gate
    if (maxConcurrent_ > 0) {
        ReleaseConcurrency();
    }

    entry->isBusy = false;

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
    return channel + ":" + chatId;
}

void SessionManager::AcquireConcurrency()
{
    std::unique_lock<std::mutex> lock(concurrencyMutex_);
    concurrencyCv_.wait(lock, [this](){ return concurrentCount_ < maxConcurrent_; });
    ++concurrentCount_;
}

void SessionManager::ReleaseConcurrency()
{
    std::lock_guard<std::mutex> lock(concurrencyMutex_);
    --concurrentCount_;
    concurrencyCv_.notify_one();
}

} // namespace jiuwen

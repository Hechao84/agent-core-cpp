#include "include/session_manager.h"
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
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

    // Create the single shared Agent
    agent_ = std::make_unique<Agent>(config_);

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
    fs::path sessionDir = fs::path(basePath) / "sessions" / SanitizePathName(sessionId);
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
    LOG(INFO) << "[SessionManager] [" << sessionId << "] Invoke requested, message length: " << message.length();

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
        LOG(ERR) << "[SessionManager] [" << sessionId << "] " << err;

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

    bool isBusy = false;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        auto it = sessions_.find(sessionId);
        if (it != sessions_.end()) {
            isBusy = it->second->isBusy;
        } else {
            // Session not in memory, but might exist on disk.
            // Proceed to delete directory if it exists.
        }
        sessionMutex_.unlock();
    }

    if (isBusy) {
        // Optional: force cancel busy session? For now, just log and delete memory.
        LOG(WARN) << "[SessionManager] Deleting busy session: " << sessionId;
    }

    // Remove from in-memory map
    // We need to lock again to perform erase
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        sessions_.erase(sessionId);
        LOG(INFO) << "[SessionManager] In-memory session removed: " << sessionId;
    }

    // Remove from disk
    if (fs::exists(sessionDir)) {
        try {
            fs::remove_all(sessionDir);
            LOG(INFO) << "[SessionManager] Session directory deleted: " << sessionDir.string();
        } catch (const std::exception& e) {
            LOG(ERR) << "[SessionManager] Failed to delete session directory: " << e.what();
        }
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

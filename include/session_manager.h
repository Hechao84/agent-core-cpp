#pragma once

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "include/agent_export.h"
#include "include/types.h"

namespace jiuwen {

class Agent;
class ContextEngine;

// Reserved session IDs for internal use
inline constexpr char kDefaultSessionId[] = "__DEFAULT__";
inline constexpr char kHeartbeatSessionId[] = "__HEARTBEAT__";
inline constexpr char kCronSessionId[] = "__CRON__";
inline constexpr char kUnifiedSessionId[] = "__UNIFIED__";

struct SessionEntry
{
    std::string sessionId;
    std::shared_ptr<ContextEngine> contextEngine;
    std::mutex invokeMutex; // Per-session lock (serializes same-session calls)
    bool isBusy{false};
    std::map<std::string, std::string> metadata; // channel, sender, etc.
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

    // Remove a session by ID and delete its data
    void RemoveSession(const std::string& sessionId);

    // Check if a session is busy
    bool IsSessionBusy(const std::string& sessionId) const;

    // Get or create a session by ID (returns ContextEngine)
    std::shared_ptr<ContextEngine> GetOrCreateSession(
        const std::string& sessionId,
        const SessionConfig& sessionConfig = {}
    );

    // Get all active session IDs
    std::vector<std::string> GetSessionIds() const;

    // Get session metadata
    std::map<std::string, std::string> GetSessionMetadata(const std::string& sessionId) const;

    // Check if initialized
    bool IsInitialized() const { return initialized_; }

    // Get the global AgentConfig
    const AgentConfig& GetConfig() const { return config_; }

    // Generate a session key from channel + chatId
    static std::string MakeSessionKey(const std::string& channel, const std::string& chatId);

private:
    SessionEntry* FindOrCreateEntry(const std::string& sessionId);
    std::shared_ptr<ContextEngine> GetContextEngine(const std::string& sessionId);
    void SetupAgentContextRouting();

    AgentConfig config_;
    bool initialized_{false};

    // Single shared Agent instance
    std::unique_ptr<Agent> agent_;

    // Per-session ContextEngine instances
    mutable std::mutex sessionMutex_;
    std::unordered_map<std::string, std::unique_ptr<SessionEntry>> sessions_;

    // Global concurrency gate
    mutable std::mutex concurrencyMutex_;
    std::condition_variable concurrencyCv_;
    int concurrentCount_{0};
    int maxConcurrent_{0};

    void AcquireConcurrency();
    void ReleaseConcurrency();
};

// Global singleton
AGENT_API SessionManager& GetSessionManager();
AGENT_API void InitSessionManager(const AgentConfig& config);

} // namespace jiuwen

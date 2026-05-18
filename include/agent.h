#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "include/agent_export.h"
#include "include/types.h"

namespace jiuwen {

class AgentWorker;
class ContextEngine; // Forward declaration
class SkillEngine;   // Forward declaration
class HistoryStore;  // Forward declaration
class DreamProcessor; // Forward declaration

struct SessionActivity
{
    std::string sessionId;
    bool isBusy{false};
};

class AGENT_API Agent {
public:
    Agent(AgentConfig config);
    ~Agent();

    // Invoke processes a query for a specific session.
    // Thread safety is provided by SessionManager (per-session mutex).
    std::string Invoke(const std::string& sessionId, const std::string& query,
                       std::function<void(const std::string&)> callback);
    bool IsSessionBusy(const std::string& sessionId) const;
    void Cancel();
    
    void AddTools(const std::vector<std::string>& toolNames);
    std::vector<std::string> GetRegisteredTools() const;

    // Notify session state changes (used by SessionManager)
    void NotifySessionIdle(const std::string& sessionId);
    void NotifySessionActive(const std::string& sessionId);
private:
    AgentConfig config_;
    std::unique_ptr<AgentWorker> worker_;
    std::shared_ptr<SkillEngine> skillEngine_;     // Skill engine is globally shared
    std::vector<std::string> toolNames_;           // Agent owns the Master Tool List

    // Per-session context engine accessor (set by SessionManager)
    // This is the mechanism that routes "which session's history/context" to use.
    std::function<std::shared_ptr<ContextEngine>(const std::string&)> contextEngineGetter_;

    void SetContextEngineGetter(std::function<std::shared_ptr<ContextEngine>(const std::string&)> getter);
    friend class SessionManager;

    // Background Dream consolidation thread
    std::thread consolidationThread_;
    mutable std::mutex consolidationMutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};

    // Dream memory consolidation (two-phase)
    std::unique_ptr<HistoryStore> historyStore_;
    std::unique_ptr<DreamProcessor> dreamProcessor_;

    // Per-session activity tracking
    mutable std::mutex sessionActivityMutex_;
    std::unordered_map<std::string, SessionActivity> sessionActivity_;

    void ConsolidationLoop();
};

} // namespace jiuwen

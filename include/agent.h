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
#include "include/skill.h"
#include "include/types.h"

namespace jiuwen {

class AgentWorker;
class ContextEngine;
class SkillEngine;
class HistoryStore;
class DreamProcessor;

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
    
    void AddTools(const std::vector<std::string>& toolNames);
    std::vector<std::string> GetRegisteredTools() const;

    std::vector<Skill> ListSkills() const;
    Skill GetSkill(const std::string& id) const;
    std::string GetSkillRootDir() const;

    const std::vector<std::string>& GetMcpServerIds() const { return config_.mcpServerIds; }
    int SyncMcpTools();

    void NotifySessionIdle(const std::string& sessionId);
    void NotifySessionActive(const std::string& sessionId);
private:
    AgentConfig config_;
    std::unique_ptr<AgentWorker> worker_;
    std::shared_ptr<SkillEngine> skillEngine_;
    std::vector<std::string> toolNames_;
    std::vector<std::string> ownedMcpTools_;

    std::function<std::shared_ptr<ContextEngine>(const std::string&)> contextEngineGetter_;

    void SetContextEngineGetter(std::function<std::shared_ptr<ContextEngine>(const std::string&)> getter);
    friend class SessionManager;

    std::thread consolidationThread_;
    mutable std::mutex consolidationMutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};

    std::unique_ptr<HistoryStore> historyStore_;
    std::unique_ptr<DreamProcessor> dreamProcessor_;

    mutable std::mutex sessionActivityMutex_;
    std::unordered_map<std::string, SessionActivity> sessionActivity_;

    void ConsolidationLoop();
};

} // namespace jiuwen

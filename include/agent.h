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

class AGENT_API Agent {
public:
    Agent(AgentConfig config);
    ~Agent();
    std::string Invoke(const std::string& query, std::function<void(const std::string&)> callback);
    void Cancel();
    void AddTools(const std::vector<std::string>& toolNames);
    std::vector<std::string> GetRegisteredTools() const;
    std::string GetMemoryContent() const;
    void UpdateMemory(const std::string& keyFacts);
    void ClearMemory();
private:
    AgentConfig config_;
    std::unique_ptr<AgentWorker> worker_;
    std::shared_ptr<ContextEngine> contextEngine_; // Store context engine
    std::shared_ptr<SkillEngine> skillEngine_;     // Store skill engine
    std::vector<std::string> toolNames_;           // Agent owns the Master Tool List

    // Sleep-time memory consolidation
    std::thread consolidationThread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};
    std::atomic<bool> isActive_{false};
    std::mutex invokeMutex_;
    
    void ConsolidateMemory();
    void ConsolidationLoop();
};

} // namespace jiuwen

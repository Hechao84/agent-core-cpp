#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "include/agent_export.h"
#include "include/types.h"

class AgentWorker;
class ContextEngine; // Forward declaration
class SkillEngine;   // Forward declaration

class AGENT_API Agent {
public:
    Agent(AgentConfig config);
    ~Agent();
    void Invoke(const std::string& query, std::function<void(const std::string&)> callback);
    void Cancel();
    void AddTools(const std::vector<std::string>& toolNames);
    std::vector<std::string> GetRegisteredTools() const;
private:
    AgentConfig config_;
    std::unique_ptr<AgentWorker> worker_;
    std::shared_ptr<ContextEngine> contextEngine_; // Store context engine
    std::shared_ptr<SkillEngine> skillEngine_;     // Store skill engine
    std::vector<std::string> toolNames_;           // Agent owns the Master Tool List
};

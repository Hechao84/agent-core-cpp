#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "include/types.h"
#include "src/tools/tool_selector.h"

namespace jiuwen {

class ContextEngine; // Forward declaration
class SkillEngine;   // Forward declaration

class AgentWorker {
public:
    AgentWorker(AgentConfig config);
    virtual ~AgentWorker() = default;
    virtual std::string Invoke(const std::string& query, std::function<void(const std::string&)> callback) = 0;
    virtual void Cancel();
    void AddTools(const std::vector<std::string>& toolNames);
    void SetContextEngine(std::shared_ptr<ContextEngine> engine);
    void SetSkillEngine(std::shared_ptr<SkillEngine> engine);
protected:
    AgentConfig config_;
    std::atomic<bool> cancelled_{false};
    std::vector<std::string> toolNames_;
    std::unique_ptr<ToolSelector> toolSelector_;
    std::shared_ptr<ContextEngine> contextEngine_;
    std::shared_ptr<SkillEngine> skillEngine_;

    void CallModelStream(const std::string& prompt, const std::vector<std::pair<std::string, std::string>>& messages,
                         std::function<void(const std::string&)> onChunk, std::function<void(const std::string&)> onComplete);
    std::string BuildPrompt(const std::string& templateName, const std::string& query, const std::string& context);
    std::string ExecuteTool(const std::string& toolName, const std::string& input);
    std::string GetToolSchemaForQuery(const std::string& query);
};

std::unique_ptr<AgentWorker> CreateAgentWorker(AgentConfig config);

} // namespace jiuwen

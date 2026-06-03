#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
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
    virtual std::string Invoke(const std::string& query, ContextEngine* contextEngine,
                               std::function<void(const std::string&)> callback) = 0;
    virtual void Cancel();
    void AddTools(const std::vector<std::string>& toolNames);
    void RemoveTools(const std::vector<std::string>& toolNames);
    void SetSkillEngine(std::shared_ptr<SkillEngine> engine);
protected:
    AgentConfig config_;
    mutable std::mutex toolMutex_;
    std::atomic<uint64_t> cancelGeneration_{0};
    std::vector<std::string> toolNames_;
    std::unique_ptr<ToolSelector> toolSelector_;
    std::shared_ptr<SkillEngine> skillEngine_;

    void CallModelStream(const std::string& prompt,
                         const std::vector<std::pair<std::string, std::string>>& messages,
                         std::function<void(const std::string&)> onChunk,
                         std::function<void(const std::string&)> onComplete,
                         uint64_t generation = 0);
    std::string BuildPrompt(const std::string& templateName, const std::string& query,
                            const std::string& context, ContextEngine* contextEngine);
    std::string ExecuteTool(const std::string& toolName, const std::string& input);
    std::string GetToolSchemaForQuery(const std::string& query);
    bool IsCancelled(uint64_t myGeneration) const;

    std::uint64_t StartNewInvocation();
};

std::unique_ptr<AgentWorker> CreateAgentWorker(AgentConfig config);

} // namespace jiuwen

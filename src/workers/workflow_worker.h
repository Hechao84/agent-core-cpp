#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "include/types.h"
#include "src/core/agent_worker.h"

namespace jiuwen {

class ContextEngine; // Forward declaration

struct WorkflowNode {
    std::string name;
    std::string promptTemplate;
    std::vector<std::string> nextNodes;
};

class WorkflowAgentWorker : public AgentWorker {
public:
    WorkflowAgentWorker(AgentConfig config);
    std::string Invoke(const std::string& query, ContextEngine* contextEngine, std::function<void(const std::string&)> callback) override;
private:
    std::vector<WorkflowNode> ParseWorkflowConfig();
    std::string ExecuteNode(const WorkflowNode& node, const std::string& input, std::function<void(const std::string&)> callback);
};

} // namespace jiuwen

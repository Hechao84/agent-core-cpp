#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "include/types.h"
#include "src/core/agent_worker.h"

struct WorkflowNode {
    std::string name;
    std::string promptTemplate;
    std::vector<std::string> nextNodes;
};

class WorkflowAgentWorker : public AgentWorker {
public:
    WorkflowAgentWorker(AgentConfig config);
    void Invoke(const std::string& query, std::function<void(const std::string&)> callback) override;
private:
    std::vector<WorkflowNode> ParseWorkflowConfig();
    std::string ExecuteNode(const WorkflowNode& node, const std::string& input, std::function<void(const std::string&)> callback);
};

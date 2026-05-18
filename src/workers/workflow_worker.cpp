#include "src/workers/workflow_worker.h"

#include <algorithm>
#include <iostream>
#include <queue>
#include <string>
#include <vector>

namespace jiuwen {

WorkflowAgentWorker::WorkflowAgentWorker(AgentConfig config) : AgentWorker(std::move(config))
{
}

std::vector<WorkflowNode> WorkflowAgentWorker::ParseWorkflowConfig()
{
    // TODO: Load from config when workflowSteps is added back
    std::vector<WorkflowNode> nodes;
    WorkflowNode defaultNode;
    defaultNode.name = "process";
    defaultNode.promptTemplate = "Process: {query}";
    nodes.push_back(defaultNode);
    return nodes;
}

std::string WorkflowAgentWorker::ExecuteNode(const WorkflowNode& node, const std::string& input, std::function<void(const std::string&)> callback)
{
    std::string promptTemplate = node.promptTemplate;
    size_t pos = promptTemplate.find("{query}");
    while (pos != std::string::npos) { 
        promptTemplate.replace(pos, 7, input); 
        pos = promptTemplate.find("{query}"); 
    }
    pos = promptTemplate.find("{input}");
    while (pos != std::string::npos) { 
        promptTemplate.replace(pos, 7, input); 
        pos = promptTemplate.find("{input}"); 
    }
    callback("[NODE] Starting: " + node.name);
    std::string fullResponse;
    CallModelStream(promptTemplate, {},
        [&callback, &fullResponse, &node](const std::string& chunk) { fullResponse += chunk; callback("[NODE_STREAM:" + node.name + "] " + chunk); },
        [&callback, &fullResponse, &node](const std::string& complete) { if (!complete.empty()) callback("[NODE_COMPLETE:" + node.name + "] " + complete); });
    return fullResponse;
}

std::string WorkflowAgentWorker::Invoke(const std::string& query, ContextEngine* /*contextEngine*/, std::function<void(const std::string&)> callback)
{
    uint64_t myGeneration = StartNewInvocation();
    std::vector<WorkflowNode> nodes = ParseWorkflowConfig();
    if (nodes.empty()) { 
        callback("[ERROR] No workflow nodes"); 
        return "";
    }
    std::string currentInput = query;
    std::queue<std::string> nodeQueue;
    nodeQueue.push(nodes[0].name);
    while (!nodeQueue.empty() && IsCancelled(myGeneration)) {
        std::string currentNodeName = nodeQueue.front();
        nodeQueue.pop();
        auto it = std::find_if(nodes.begin(), nodes.end(), 
                [&currentNodeName](const WorkflowNode& n) { return n.name == currentNodeName; });
        if (it == nodes.end()) { 
            callback("[ERROR] Node not found: " + currentNodeName); 
            continue; 
        }
        currentInput = ExecuteNode(*it, currentInput, callback);
        if (!IsCancelled(myGeneration)) { 
            callback("[STATUS] Cancelled"); 
            return "";
        }
        for (const auto& nextNode : it->nextNodes) nodeQueue.push(nextNode);
    }
    if (!IsCancelled(myGeneration)) {
        callback("[STATUS] Cancelled");
        return "";
    }
    callback("[FINAL] " + currentInput);
    return currentInput;
}

} // namespace jiuwen

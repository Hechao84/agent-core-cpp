#include "react_worker.h"
#include <iostream>
#include <algorithm>

ReactAgentWorker::ReactAgentWorker(AgentConfig config) : AgentWorker(std::move(config)) {}

std::string ReactAgentWorker::ParseThought(const std::string& response) {
    size_t start = response.find("Thought:");
    if (start == std::string::npos) return "";
    size_t end = response.find("\n", start);
    if (end == std::string::npos) end = response.find("Action:", start);
    if (end == std::string::npos) return response.substr(start + 8);
    return response.substr(start + 8, end - start - 8);
}

std::string ReactAgentWorker::ParseAction(const std::string& response) {
    size_t start = response.find("Action:");
    if (start == std::string::npos) return "";
    size_t end = response.find("\n", start);
    if (end == std::string::npos) return response.substr(start + 7);
    return response.substr(start + 7, end - start - 7);
}

std::string ReactAgentWorker::ParseActionInput(const std::string& response) {
    size_t start = response.find("Action Input:");
    if (start == std::string::npos) return "";
    size_t end = response.find("\n", start);
    if (end == std::string::npos) return response.substr(start + 13);
    return response.substr(start + 13, end - start - 13);
}

void ReactAgentWorker::ReactLoop(const std::string& query, std::function<void(const std::string&)> callback) {
    std::string scratchpad;
    for (int iteration = 0; iteration < m_config.maxIterations && !m_cancelled.load(); ++iteration) {
        std::string prompt = BuildPrompt("react_system", query, scratchpad);
        callback("[STATUS] Thinking... (Iteration " + std::to_string(iteration + 1) + ")");
        std::string fullResponse;
        CallModelStream(prompt, {}, 
            [&callback, &fullResponse](const std::string& chunk) { fullResponse += chunk; callback("[STREAM] " + chunk); },
            [&callback, &fullResponse](const std::string& complete) { if (!complete.empty()) callback("[RESPONSE] " + complete); });
        if (m_cancelled.load()) { callback("[STATUS] Cancelled"); return; }
        std::string thought = ParseThought(fullResponse);
        std::string action = ParseAction(fullResponse);
        std::string actionInput = ParseActionInput(fullResponse);
        if (!thought.empty()) callback("[THOUGHT] " + thought);
        if (action.empty()) { callback("[FINAL] " + fullResponse); return; }
        callback("[ACTION] " + action);
        if (!actionInput.empty()) callback("[ACTION_INPUT] " + actionInput);
        std::string observation = ExecuteTool(action, actionInput);
        callback("[OBSERVATION] " + observation);
        scratchpad += "\nThought: " + thought + "\nAction: " + action + "\nAction Input: " + actionInput + "\nObservation: " + observation;
    }
    if (m_cancelled.load()) callback("[STATUS] Cancelled");
    else callback("[STATUS] Max iterations reached");
}

void ReactAgentWorker::Invoke(const std::string& query, std::function<void(const std::string&)> callback) {
    m_cancelled.store(false);
    ReactLoop(query, std::move(callback));
}

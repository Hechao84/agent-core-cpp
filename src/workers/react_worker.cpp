#include "src/workers/react_worker.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "src/context_engine/context_engine.h"
#include "src/tools/tool_selector.h"
#include "src/utils/logger.h"
#include "src/utils/tool_parser.h"

namespace jiuwen {

ReactAgentWorker::ReactAgentWorker(AgentConfig config) : AgentWorker(std::move(config))
{
}

std::string ReactAgentWorker::ReactLoop(const std::string& query, ContextEngine* contextEngine, std::function<void(const std::string&)> callback, uint64_t myGeneration)
{
    LOG(INFO) << "Starting ReactLoop for query: " << query;

    std::string scratchpad;
    std::string finalAnswer;

    std::vector<std::pair<std::string, std::string>> msgHistory;
    if (contextEngine) {
        auto history = contextEngine->GetContextWindow();
        LOG(INFO) << "Loaded " << history.size() << " messages from context history.";
        for (const auto& m : history) {
            msgHistory.push_back({m.role, m.content});
        }
    }

    for (int iteration = 0; iteration < config_.maxIterations; ++iteration) {
        if (!IsCancelled(myGeneration)) {
            callback("\n[STATUS] Cancelled\n");
            return finalAnswer;
        }

        std::string prompt = BuildPrompt("react_system", query, scratchpad, contextEngine);
        callback("\n[STATUS] Thinking... (Iteration " + std::to_string(iteration + 1) + ")\n");

        std::string fullResponse;

        CallModelStream(prompt, msgHistory,
            [&callback, &fullResponse](const std::string& chunk)
            {
                fullResponse += chunk;
                if (!chunk.empty()) callback("[STREAM] " + chunk);
            },
            [](const std::string& complete) { (void)complete; }, myGeneration);

        if (fullResponse.empty()) {
            LOG(WARN) << "[React] Model returned empty response. Loop stopped.";
            callback("\n[STATUS] Model returned empty response\n");
            return "";
        }

        if (!IsCancelled(myGeneration)) { 
            callback("\n[STATUS] Cancelled\n"); 
            return finalAnswer;
        }

        std::vector<ToolCall> toolCalls = ExtractAllToolCalls(fullResponse);

        if (toolCalls.empty()) {
            msgHistory.push_back({"assistant", fullResponse});
            LOG(INFO) << "No tool action parsed, treating as final response.";

            std::string cleanAnswer = TrimStr(fullResponse);
            callback("\n[FINAL] " + cleanAnswer + "\n");
            return cleanAnswer;
        }

        std::string combinedObservation;
        for (const auto& toolCall : toolCalls) {
            LOG(INFO) << "Parsed tool call: " << toolCall.name << " with input: " << toolCall.arguments;
            callback("\n[TOOL_CALLS] {\"name\": \"" + toolCall.name + "\", \"arguments\": " + toolCall.arguments + "}\n");

            std::string observation = ExecuteTool(toolCall.name, toolCall.arguments);
            LOG(INFO) << "[React] Tool observation length: " << observation.length();
            callback("\n[TOOL_RESPONSE]" + observation + "\n");

            std::string assistantMsg = "{\"name\": \"" + toolCall.name + "\", \"arguments\": " + toolCall.arguments + "}";
            msgHistory.push_back({"assistant", assistantMsg});
            msgHistory.push_back({"tool", observation + "\nInput was: " + toolCall.arguments});

            if (!combinedObservation.empty()) {
                combinedObservation += "\n\n---\n\n";
            }
            combinedObservation += "Tool: " + toolCall.name + "\nInput: " + toolCall.arguments + "\nResult: " + observation;
        }

        scratchpad += "\nThought: [Streamed]\n";
        for (const auto& toolCall : toolCalls) {
            scratchpad += "Action: " + toolCall.name + "\nAction Input: " + toolCall.arguments + "\n";
        }
        scratchpad += "Observation: " + combinedObservation + "\n";
    }

    callback("\n[STATUS] Max iterations reached\n");

    return finalAnswer;
}

std::string ReactAgentWorker::Invoke(const std::string& query, ContextEngine* contextEngine, std::function<void(const std::string&)> callback)
{
    uint64_t myGeneration = StartNewInvocation();
    return ReactLoop(query, contextEngine, std::move(callback), myGeneration);
}

} // namespace jiuwen

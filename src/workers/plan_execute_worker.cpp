#include "src/workers/plan_execute_worker.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace jiuwen {

PlanAndExecuteAgentWorker::PlanAndExecuteAgentWorker(AgentConfig config) : AgentWorker(std::move(config))
{
}

std::vector<std::string> PlanAndExecuteAgentWorker::GeneratePlan(const std::string& query, ContextEngine* contextEngine, std::function<void(const std::string&)> callback, uint64_t myGeneration)
{
    std::string prompt = BuildPrompt("plan_system", query, "", contextEngine);
    callback("[STATUS] Generating plan...");
    std::string fullResponse;
    CallModelStream(prompt, {},
        [&callback, &fullResponse](const std::string& chunk) { fullResponse += chunk; callback("[PLAN_STREAM] " + chunk); },
        [&callback, &fullResponse](const std::string& complete) { if (!complete.empty()) callback("[PLAN_COMPLETE] " + complete); },
        myGeneration);
    if (!IsCancelled(myGeneration)) return {};
    std::vector<std::string> steps;
    std::istringstream stream(fullResponse);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            size_t numPos = line.find_first_of("0123456789");
            if (numPos != std::string::npos) {
                size_t start = line.find_first_not_of(" \t.", numPos + 1);
                if (start != std::string::npos) steps.push_back(line.substr(start));
            }
        }
    }
    if (steps.empty()) steps.push_back(fullResponse);
    callback("[STATUS] Plan generated with " + std::to_string(steps.size()) + " steps");
    return steps;
}

std::string PlanAndExecuteAgentWorker::ExecuteStep(const std::string& step, const std::string& context, ContextEngine* contextEngine, std::function<void(const std::string&)> callback, uint64_t myGeneration)
{
    std::string prompt = BuildPrompt("execute_system", step, context, contextEngine);
    callback("[STATUS] Executing step: " + step);
    std::string fullResponse;
    CallModelStream(prompt, {},
        [&callback, &fullResponse](const std::string& chunk) { fullResponse += chunk; callback("[STEP_STREAM] " + chunk); },
        [&callback, &fullResponse](const std::string& complete) { if (!complete.empty()) callback("[STEP_COMPLETE] " + complete); },
        myGeneration);
    return fullResponse;
}

std::string PlanAndExecuteAgentWorker::SynthesizeResult(const std::string& query, const std::string& context, ContextEngine* contextEngine, std::function<void(const std::string&)> callback, uint64_t myGeneration)
{
    std::string prompt = BuildPrompt("synthesize_system", query, context, contextEngine);
    callback("[STATUS] Synthesizing final result...");
    std::string fullResponse;
    CallModelStream(prompt, {},
        [&callback, &fullResponse](const std::string& chunk) { fullResponse += chunk; callback("[RESULT_STREAM] " + chunk); },
        [&callback, &fullResponse](const std::string& complete) { if (!complete.empty()) callback("[FINAL] " + complete); },
        myGeneration);
    return fullResponse;
}

std::string PlanAndExecuteAgentWorker::Invoke(const std::string& query, ContextEngine* contextEngine, std::function<void(const std::string&)> callback)
{
    uint64_t myGeneration = StartNewInvocation();
    std::vector<std::string> plan = GeneratePlan(query, contextEngine, callback, myGeneration);
    if (!IsCancelled(myGeneration) || plan.empty()) { 
        callback("[STATUS] Cancelled or empty plan"); 
        return "";
    }
    std::string context;
    for (size_t i = 0; i < plan.size() && IsCancelled(myGeneration); ++i) {
        callback("[PROGRESS] Step " + std::to_string(i + 1) + "/" + std::to_string(plan.size()));
        std::string stepResult = ExecuteStep(plan[i], context, contextEngine, callback, myGeneration);
        context += "\nStep " + std::to_string(i + 1) + ": " + plan[i] + "\nResult: " + stepResult;
    }
    if (!IsCancelled(myGeneration)) { 
        callback("[STATUS] Cancelled during execution"); 
        return "";
    }
    return SynthesizeResult(query, context, contextEngine, callback, myGeneration);
}

} // namespace jiuwen

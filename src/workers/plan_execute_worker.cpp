
#include "src/workers/plan_execute_worker.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

PlanAndExecuteAgentWorker::PlanAndExecuteAgentWorker(AgentConfig config) : AgentWorker(std::move(config)){} std::vector<std::string> PlanAndExecuteAgentWorker::GeneratePlan(const std::string& query, std::function<void(const std::string&)> callback)
{
    std::string prompt = BuildPrompt("plan_system", query, "");
    callback("[STATUS] Generating plan...");
    std::string fullResponse;
    CallModelStream(prompt, {},
        [&callback, &fullResponse](const std::string& chunk) { fullResponse += chunk; callback("[PLAN_STREAM] " + chunk); },
        [&callback, &fullResponse](const std::string& complete) { if (!complete.empty()) callback("[PLAN_COMPLETE] " + complete); });
    if (cancelled_.load()) return {};
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

std::string PlanAndExecuteAgentWorker::ExecuteStep(const std::string& step, const std::string& context, std::function<void(const std::string&)> callback)
{
    std::string prompt = BuildPrompt("execute_system", step, context);
    callback("[STATUS] Executing step: " + step);
    std::string fullResponse;
    CallModelStream(prompt, {},
        [&callback, &fullResponse](const std::string& chunk) { fullResponse += chunk; callback("[STEP_STREAM] " + chunk); },
        [&callback, &fullResponse](const std::string& complete) { if (!complete.empty()) callback("[STEP_COMPLETE] " + complete); });
    return fullResponse;
}

std::string PlanAndExecuteAgentWorker::SynthesizeResult(const std::string& query, const std::string& context, std::function<void(const std::string&)> callback)
{
    std::string prompt = BuildPrompt("synthesize_system", query, context);
    callback("[STATUS] Synthesizing final result...");
    std::string fullResponse;
    CallModelStream(prompt, {},
        [&callback, &fullResponse](const std::string& chunk) { fullResponse += chunk; callback("[RESULT_STREAM] " + chunk); },
        [&callback, &fullResponse](const std::string& complete) { if (!complete.empty()) callback("[FINAL] " + complete); });
    return fullResponse;
}

std::string PlanAndExecuteAgentWorker::Invoke(const std::string& query, std::function<void(const std::string&)> callback)
{
    cancelled_.store(false);
    std::vector<std::string> plan = GeneratePlan(query, callback);
    if (cancelled_.load() || plan.empty()) { 
        callback("[STATUS] Cancelled or empty plan"); 
        return "";
    }
    std::string context;
    for (size_t i = 0; i < plan.size() && !cancelled_.load(); ++i) {
        callback("[PROGRESS] Step " + std::to_string(i + 1) + "/" + std::to_string(plan.size()));
        std::string stepResult = ExecuteStep(plan[i], context, callback);
        context += "\nStep " + std::to_string(i + 1) + ": " + plan[i] + "\nResult: " + stepResult;
    }
    if (cancelled_.load()) { 
        callback("[STATUS] Cancelled during execution"); 
        return "";
    }
    return SynthesizeResult(query, context, callback);
}

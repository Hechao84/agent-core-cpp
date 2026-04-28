#include "src/workers/react_worker.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>
#include "src/utils/logger.h"
#include "src/context_engine/context_engine.h"
#include "src/tools/tool_selector.h"

namespace jiuwen {

static std::string ExtractJson(const std::string& text, size_t startPos)
{
    if (startPos >= text.length()) return "";
    if (text[startPos] != '{') return "";

    int depth = 0;
    bool inString = false;
    for (size_t i = startPos; i < text.length(); ++i) {
        if (text[i] == '\\') { 
            i++; 
            continue; 
        }
        if (text[i] == '"') inString = !inString;
        if (!inString) {
            if (text[i] == '{') {
                depth++;
            } else if (text[i] == '}') {
                depth--;
                if (depth == 0) return text.substr(startPos, i - startPos + 1);
            }
        }
    }
    return "";
}

struct ToolCall {
    std::string name;
    std::string arguments;
};

// Extract all JSON objects that contain "name" and "arguments" fields
static std::vector<ToolCall> ExtractAllToolCalls(const std::string& response)
{
    std::vector<ToolCall> calls;
    size_t searchPos = 0;

    while (searchPos < response.length()) {
        size_t jsonStart = response.find('{', searchPos);
        if (jsonStart == std::string::npos) break;

        std::string jsonStr = ExtractJson(response, jsonStart);
        if (jsonStr.empty()) {
            searchPos = jsonStart + 1;
            continue;
        }

        // Check if this JSON has "name" and "arguments" fields
        size_t nameKey = jsonStr.find("\"name\"");
        size_t argsKey = jsonStr.find("\"arguments\"");
        if (nameKey != std::string::npos && argsKey != std::string::npos) {
            ToolCall call;
            call.arguments = "{}";

            // Extract name value
            size_t colon = jsonStr.find(':', nameKey + 6);
            size_t valStart = jsonStr.find_first_not_of(" \t", colon + 1);
            if (valStart != std::string::npos && jsonStr[valStart] == '"') {
                size_t valEnd = jsonStr.find('"', valStart + 1);
                if (valEnd != std::string::npos) {
                    call.name = jsonStr.substr(valStart + 1, valEnd - valStart - 1);
                }
            }

            // Extract arguments value
            colon = jsonStr.find(':', argsKey + 11);
            valStart = jsonStr.find_first_not_of(" \t", colon + 1);
            if (valStart != std::string::npos && valStart < jsonStr.length()) {
                if (jsonStr[valStart] == '{') {
                    std::string argsObj = ExtractJson(jsonStr, valStart);
                    if (!argsObj.empty()) {
                        call.arguments = argsObj;
                    }
                } else if (jsonStr[valStart] == '"') {
                    size_t aEnd = jsonStr.find('"', valStart + 1);
                    if (aEnd != std::string::npos) {
                        call.arguments = "\"" + jsonStr.substr(valStart + 1, aEnd - valStart - 1) + "\"";
                    }
                }
            }

            // Validate name
            if (!call.name.empty() && call.name.length() < 50 &&
                call.name.find('{') == std::string::npos &&
                call.name.find('}') == std::string::npos) {
                calls.push_back(call);
            }
        }

        searchPos = jsonStart + jsonStr.length();
    }

    return calls;
}

// Parse tool call from model response. Handles both:
// 1. JSON format: {"name": "weather", "arguments": {"city": "Beijing"}}
// 2. ReAct format: Action: weather\nAction Input: {"city": "Beijing"}
static std::string ParseAction(const std::string& response, std::string& actionInput)
{
    actionInput = "{}";

    // Strategy 1: Try to find JSON with "name" and "arguments" fields
    size_t jsonStart = response.find('{');
    if (jsonStart != std::string::npos) {
        std::string jsonStr = ExtractJson(response, jsonStart);
        if (!jsonStr.empty()) {
            // Extract "name" value
            size_t nameKey = jsonStr.find("\"name\"");
            if (nameKey != std::string::npos) {
                size_t colon = jsonStr.find(':', nameKey + 6);
                size_t valStart = jsonStr.find_first_not_of(" \t", colon + 1);
                if (valStart != std::string::npos && jsonStr[valStart] == '"') {
                    size_t valEnd = jsonStr.find('"', valStart + 1);
                    if (valEnd != std::string::npos) {
                        std::string name = jsonStr.substr(valStart + 1, valEnd - valStart - 1);
                        // Validate: name should be short, not contain spaces or braces
                        if (!name.empty() && name.length() < 50 &&
                            name.find('{') == std::string::npos &&
                            name.find('}') == std::string::npos) {
                            // Extract "arguments" value (object or string)
                            size_t argsKey = jsonStr.find("\"arguments\"");
                            if (argsKey != std::string::npos) {
                                size_t aColon = jsonStr.find(':', argsKey + 11);
                                size_t aValStart = jsonStr.find_first_not_of(" \t", aColon + 1);
                                if (aValStart != std::string::npos && aValStart < jsonStr.length()) {
                                    if (jsonStr[aValStart] == '{') {
                                        std::string argsObj = ExtractJson(jsonStr, aValStart);
                                        if (!argsObj.empty()) {
                                            actionInput = argsObj;
                                        }
                                    } else if (jsonStr[aValStart] == '"') {
                                        size_t aEnd = jsonStr.find('"', aValStart + 1);
                                        if (aEnd != std::string::npos) {
                                            actionInput = "\"" + jsonStr.substr(aValStart + 1, aEnd - aValStart - 1) + "\"";
                                        }
                                    }
                                }
                            }
                            return name;
                        }
                    }
                }
            }
        }
    }

    // Strategy 2: Classic ReAct format
    size_t actPos = response.find("Action:");
    if (actPos != std::string::npos) {
        size_t end = response.find('\n', actPos);
        std::string actLine = response.substr(actPos + 7, end - actPos - 7);

        // Trim whitespace
        size_t f = actLine.find_first_not_of(" \t\r\n");
        if (f != std::string::npos) actLine.erase(0, f);
        size_t l = actLine.find_last_not_of(" \t\r\n");
        if (l != std::string::npos) actLine.erase(l + 1);

        // If Action line itself is a JSON, recurse
        if (!actLine.empty() && actLine.front() == '{') {
            std::string multiLine = response.substr(actPos);
            return ParseAction(multiLine, actionInput);
        }

        std::string name = actLine;
        // Remove leading braces if present
        if (!name.empty() && name.front() == '{') {
            size_t braceEnd = name.find('}');
            if (braceEnd != std::string::npos) name.erase(0, braceEnd + 1);
        }
        // Remove trailing braces
        if (!name.empty() && name.back() == '}') {
            size_t braceStart = name.rfind('{');
            if (braceStart != std::string::npos) name.erase(braceStart);
        }
        // Trim again
        f = name.find_first_not_of(" \t\r\n");
        if (f != std::string::npos) name.erase(0, f);
        l = name.find_last_not_of(" \t\r\n");
        if (l != std::string::npos) name.erase(l + 1);

        // Extract Action Input
        size_t inputPos = response.find("Action Input:");
        if (inputPos != std::string::npos) {
            size_t inputEnd = response.find('\n', inputPos);
            std::string input = response.substr(inputPos + 13, inputEnd - inputPos - 13);
            f = input.find_first_not_of(" \t\r\n");
            if (f != std::string::npos) input.erase(0, f);
            l = input.find_last_not_of(" \t\r\n");
            if (l != std::string::npos) input.erase(l + 1);
            if (!input.empty()) {
                if (input.front() == '{') actionInput = input;
                else actionInput = "{\"input\": \"" + input + "\"}";
            }
        }

        return name;
    }

    return "";
}

static std::string TrimStr(const std::string& str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

ReactAgentWorker::ReactAgentWorker(AgentConfig config) : AgentWorker(std::move(config)){}

std::string ReactAgentWorker::ReactLoop(const std::string& query, std::function<void(const std::string&)> callback)
{
    LOG(INFO) << "Starting ReactLoop for query: " << query;

    std::string scratchpad;
    std::string finalAnswer;

    std::vector<std::pair<std::string, std::string>> msgHistory;
    if (contextEngine_) {
        auto history = contextEngine_->GetContextWindow();
        LOG(INFO) << "Loaded " << history.size() << " messages from context history.";
        for (const auto& m : history) {
            msgHistory.push_back({m.role, m.content});
        }
    }

    for (int iteration = 0; iteration < config_.maxIterations && !cancelled_.load(); ++iteration) {
        std::string prompt = BuildPrompt("react_system", query, scratchpad);
        callback("\n[STATUS] Thinking... (Iteration " + std::to_string(iteration + 1) + ")\n");

        std::string fullResponse;

        CallModelStream(prompt, msgHistory,
            [&callback, &fullResponse](const std::string& chunk)
            {
                fullResponse += chunk;
                if (!chunk.empty()) callback("[STREAM] " + chunk);
            },
            [](const std::string& complete) { (void)complete; });

        if (fullResponse.empty()) {
            LOG(WARN) << "[React] Model returned empty response. Loop stopped.";
            callback("\n[STATUS] Model returned empty response\n");
            return "";
        }

        if (cancelled_.load()) { 
            callback("\n[STATUS] Cancelled\n"); 
            return finalAnswer;
        }

        std::vector<ToolCall> toolCalls = ExtractAllToolCalls(fullResponse);

        if (toolCalls.empty()) {
            // Final response, add to history and return
            msgHistory.push_back({"assistant", fullResponse});
            LOG(INFO) << "No tool action parsed, treating as final response.";

            // fullResponse contains only raw LLM output (tags are added by callback, stored separately)
            std::string cleanAnswer = TrimStr(fullResponse);
            callback("\n[FINAL] " + cleanAnswer + "\n");
            return cleanAnswer;
        }

        // Execute all tool calls and collect observations
        std::string combinedObservation;
        for (const auto& toolCall : toolCalls) {
            LOG(INFO) << "Parsed tool call: " << toolCall.name << " with input: " << toolCall.arguments;
            callback("\n[TOOL_CALLS] {\"name\": \"" + toolCall.name + "\", \"arguments\": " + toolCall.arguments + "}\n");

            std::string observation = ExecuteTool(toolCall.name, toolCall.arguments);
            LOG(INFO) << "[React] Tool observation length: " << observation.length();
            callback("\n[TOOL_RESPONSE]" + observation + "\n");

            std::string assistantMsg = "{\"name\": \"" + toolCall.name + "\", \"arguments\": " + toolCall.arguments + "}";
            msgHistory.push_back({"assistant", assistantMsg});
            // Add tool output to history so the model sees the result (or error)
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

    if (!cancelled_.load()) {
        callback("\n[STATUS] Max iterations reached\n");
    }

    return finalAnswer;
}

std::string ReactAgentWorker::Invoke(const std::string& query, std::function<void(const std::string&)> callback)
{
    cancelled_.store(false);
    return ReactLoop(query, std::move(callback));
}

} // namespace jiuwen

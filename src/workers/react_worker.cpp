#include "src/workers/react_worker.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "include/memory_runtime.h"
#include "src/context_engine/context_engine.h"
#include "src/core/worker_env.h"
#include "src/utils/logger.h"
#include "src/utils/tool_parser.h"
#include "third_party/include/nlohmann/json.hpp"

using json = nlohmann::json;

namespace jiuwen {

namespace {

// Local id counter for fallback (prompt-only) tool-calls. In native mode
// the server returns ids; in fallback the worker assigns simple stable ids.
std::atomic<uint64_t> g_fallbackCounter{0};

std::string MakeFallbackCallId(const std::string& sessionId)
{
    uint64_t n = g_fallbackCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    return std::string("call_") + (sessionId.empty() ? std::string("anon") : sessionId) + "_" + std::to_string(n);
}

// Build a compact JSON payload describing a tool call for SSE streaming.
std::string EncodeToolCallTag(const ToolCall& tc)
{
    json j;
    j["id"] = tc.id;
    j["name"] = tc.name;
    // arguments is already a JSON-encoded string; embed verbatim if valid.
    try {
        j["arguments"] = json::parse(tc.argumentsJson);
    } catch (...) {
        j["arguments"] = tc.argumentsJson;
    }
    return j.dump();
}

} // namespace

ReactAgentWorker::ReactAgentWorker(AgentConfig config) : AgentWorker(std::move(config))
{
}

std::string ReactAgentWorker::ReactLoop(const std::string& query, ContextEngine* contextEngine,
                                         std::function<void(const std::string&)> callback,
                                         uint64_t myGeneration)
{
    LOG(INFO) << "[React] Starting loop for query length=" << query.length();

    // Load history (already includes the just-added user message) so the
    // model sees the running conversation.
    std::vector<Message> msgHistory;
    if (contextEngine) {
        msgHistory = contextEngine->GetContextWindow();
        LOG(INFO) << "[React] Loaded " << msgHistory.size() << " messages from history";
    }

    std::string scratchpad;  // unused under structured mode but kept for BuildPrompt signature

    for (int iteration = 0; iteration < config_.maxIterations; ++iteration) {
        if (!IsCancelled(myGeneration)) {
            callback("\n[STATUS] Cancelled\n");
            return "";
        }

        std::string systemPrompt = BuildPrompt("react_system", query, scratchpad, contextEngine);
        callback("\n[STATUS] Thinking... (Iteration " + std::to_string(iteration + 1) + ")\n");

        ModelResponse resp = CallModelStream(
            systemPrompt,
            msgHistory,
            [&callback](const std::string& chunk) {
                if (!chunk.empty()) callback("[STREAM]" + chunk);
            },
            myGeneration);

        if (resp.finishReason == "error") {
            callback("\n[STATUS] " + resp.content + "\n");
            return "";
        }
        if (resp.finishReason == "cancelled" || !IsCancelled(myGeneration)) {
            callback("\n[STATUS] Cancelled\n");
            return "";
        }

        // Fallback (prompt-only) mode: parse tool calls out of resp.content
        // using the legacy ReAct tool_parser so older endpoints still work.
        if (resp.toolCalls.empty() && !config_.modelConfig.useNativeFunctionCalling) {
            auto parsed = ExtractAllToolCalls(resp.content);
            for (const auto& p : parsed) {
                ToolCall tc;
                tc.id = MakeFallbackCallId(config_.contextConfig.sessionId);
                tc.name = p.name;
                tc.argumentsJson = p.arguments.empty() ? "{}" : p.arguments;
                resp.toolCalls.push_back(std::move(tc));
            }
        }

        // Terminal case: model produced no tool calls -> treat content as
        // the final answer.
        if (resp.toolCalls.empty()) {
            std::string finalAnswer = TrimStr(resp.content);
            if (finalAnswer.empty()) {
                LOG(WARN) << "[React] Empty response with no tool_calls; stopping loop";
                callback("\n[STATUS] Model returned empty response\n");
                return "";
            }
            // Persist the final assistant turn (text-only).
            Message asst;
            asst.role = "assistant";
            asst.content = finalAnswer;
            msgHistory.push_back(asst);
            if (contextEngine) contextEngine->AddMessage(asst);

            callback("\n[FINAL] " + finalAnswer + "\n");
            return finalAnswer;
        }

        // Build the structured assistant message (may have both content and
        // tool_calls -- some models emit thinking text alongside calls).
        Message asst;
        asst.role = "assistant";
        asst.content = resp.content;
        asst.toolCalls = resp.toolCalls;
        msgHistory.push_back(asst);
        if (contextEngine) contextEngine->AddMessage(asst);

        // Execute each tool call sequentially and emit per-call SSE tags.
        for (const auto& tc : resp.toolCalls) {
            LOG(INFO) << "[React] Executing tool " << tc.name << " (id=" << tc.id << ")";
            callback("\n[TOOL_CALLS] " + EncodeToolCallTag(tc) + "\n");

            std::string observation = ExecuteTool(tc.name, tc.argumentsJson, callback);
            LOG(INFO) << "[React] Tool " << tc.name << " produced " << observation.length() << " chars";

            // Tag includes the call id so the front-end can route the
            // response back to the correct bubble.
            callback("\n[TOOL_RESPONSE " + tc.id + "] " + observation + "\n");

            Message tool;
            tool.role = "tool";
            tool.toolCallId = tc.id;
            tool.toolName = tc.name;
            tool.content = observation;
            if (workerEnv_ && contextEngine) {
                MemoryRuntime* memoryRuntime = workerEnv_->GetMemoryRuntime();
                if (memoryRuntime) {
                    MemoryPayloadWriteRequest request;
                    request.agentId = config_.id;
                    request.sessionId = contextEngine->GetSessionId();
                    request.content = observation;
                    request.contentType = "tool_result";
                    request.toolCallId = tc.id;
                    request.toolName = tc.name;
                    MemoryPayloadWriteResult payloadResult = memoryRuntime->WritePayload(request);
                    if (payloadResult.offloaded) {
                        tool.content = payloadResult.replacementContent;
                        tool.payloadRef = payloadResult.payload.ref;
                    }
                }
            }
            msgHistory.push_back(tool);
            if (contextEngine) contextEngine->AddMessage(tool);
        }
        // Loop back: the model will see the just-added tool results on the
        // next CallModelStream pass.
    }

    callback("\n[STATUS] Max iterations reached\n");
    return "";
}

std::string ReactAgentWorker::Invoke(const std::string& query, ContextEngine* contextEngine,
                                      std::function<void(const std::string&)> callback)
{
    uint64_t myGeneration = StartNewInvocation();
    return ReactLoop(query, contextEngine, std::move(callback), myGeneration);
}

} // namespace jiuwen

#include "src/workers/react_worker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "include/memory_runtime.h"
#include "src/context_engine/context_engine.h"
#include "src/core/capability_selector.h"
#include "src/core/turn_state.h"
#include "src/core/worker_env.h"
#include "src/skills/skill_engine.h"
#include "src/utils/logger.h"
#include "src/utils/time_utils.h"
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

// Build the per-iteration Runtime Note appended at the tail of the
// messages array. Kept out of the system prompt so the prompt prefix
// (agents / soul / user / tools / memory / skills / tools catalog) stays
// byte-stable across iterations and KV-cache friendly. Injecting the
// current time at the tail instead has two effects:
//   1. The system message becomes a stable cache prefix (the previous
//      in-prompt `Current Time` field broke prefix-cache on every second
//      boundary).
//   2. The tail position is where the model's recency-attention is
//      strongest, which suppresses a known failure mode where stale
//      `time_info` tool results from earlier turns get treated as
//      "today".
std::string BuildRuntimeNote()
{
    std::string s;
    s += "[Runtime Note]\n";
    s += "Current Time: " + jiuwen::NowLocalHumanReadable() + " (Local Time)\n";
    s += "Today's Date: " + jiuwen::NowLocalDateWithWeekday() + "\n";
    s += "UTC Time: " + jiuwen::NowUtcIso8601() + "\n";
    s += "Note: Tool results earlier in this conversation may reference older "
         "dates/times. Treat the time above as authoritative for any "
         "\"today\" / \"now\" references in the user's query.";
    return s;
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

    std::string scratchpad;  // unused under structured mode but kept for BuildPrompt signature

    // msgHistory is refreshed from the ContextEngine at the top of every
    // iteration rather than accumulated locally. Each iteration persists its
    // assistant/tool turns via AddMessage(), so the next GetContextWindow()
    // already reflects them AND applies the context-window limits. This keeps
    // what we send to the model bounded by maxContextTokens/maxMessages, even
    // across many tool-calling rounds (previously the local copy grew without
    // bound and could overflow the model's context). The latest user query is
    // always retained: it is the start of the most-recent segment, which the
    // limiter selects first and preserves even when compressing.
    std::vector<Message> msgHistory;
    if (!contextEngine) {
        LOG(ERR) << "[React] No ContextEngine; aborting loop";
        callback("\n[STATUS] Error: no context engine\n");
        return "";
    }

    for (int iteration = 0; iteration < config_.maxIterations; ++iteration) {
        if (IsCancelled(myGeneration)) {
            callback("\n[STATUS] Cancelled\n");
            return "";
        }

        msgHistory = contextEngine->GetContextWindow();
        LOG(INFO) << "[React] Iteration " << (iteration + 1) << ": loaded "
                  << msgHistory.size() << " messages from context window";

        std::string systemPrompt = BuildPrompt("react_system", query, scratchpad, contextEngine);
        callback("\n[STATUS] Thinking... (Iteration " + std::to_string(iteration + 1) + ")\n");

        // Inject the per-iteration Runtime Note at the tail of the messages
        // array. msgHistory is a by-value copy of the context window, so this
        // mutation never reaches ContextEngine's persistence. The note is a
        // role=system message (legal at any position under the OpenAI spec;
        // AnthropicModel::Format folds non-leading system messages into the
        // top-level system field to satisfy Anthropic's protocol).
        Message runtimeNote;
        runtimeNote.role = "system";
        runtimeNote.content = BuildRuntimeNote();
        msgHistory.push_back(runtimeNote);

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
        if (resp.finishReason == "cancelled" || IsCancelled(myGeneration)) {
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

        for (auto& tc : resp.toolCalls) {
            if (tc.id.empty()) {
                tc.id = MakeFallbackCallId(contextEngine ? contextEngine->GetSessionId() : config_.contextConfig.sessionId);
                LOG(WARN) << "[React] Generated missing tool_call id=" << tc.id << " tool=" << tc.name;
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
            contextEngine->AddMessage(asst);

            callback("\n[FINAL] " + finalAnswer + "\n");
            return finalAnswer;
        }

        // Build the structured assistant message (may have both content and
        // tool_calls -- some models emit thinking text alongside calls).
        Message asst;
        asst.role = "assistant";
        asst.content = resp.content;
        asst.toolCalls = resp.toolCalls;
        contextEngine->AddMessage(asst);

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
                        tool.payloadRef = payloadResult.payload.uri;
                    } else if (!payloadResult.succeeded) {
                        LOG(WARN) << "[ReactWorker] WritePayload failed for tool=" << tc.name
                                  << " sessionId=" << contextEngine->GetSessionId()
                                  << " (large content not offloaded)";
                    }
                }
            }
            contextEngine->AddMessage(tool);
        }
        // Loop back: the next iteration reloads the (now-updated, limited)
        // context window via GetContextWindow().
    }

    callback("\n[STATUS] Max iterations reached\n");
    return "";
}

std::string ReactAgentWorker::Invoke(const std::string& query, ContextEngine* contextEngine,
                                       std::function<void(const std::string&)> callback)
{
    uint64_t myGeneration = CurrentCancelGeneration();

    // V2 (round5 §5.4.1 条 3-7 + 条 9): per-turn capability disclosure state
    // setup. Per-side seed strategy 解法 X — compare findRelevant output to
    // the full pool at the Invoke entry (the only point with both pieces in
    // scope; proxy doesn't hold pool reference). Branches:
    //   - SELECTIVE + non-empty findRelevant subset → seedActiveSubset
    //     (flag=false → search routes to real recall)
    //   - SELECTIVE + findRelevant returns == pool (全相关) → seedActive
    //     (flag=true → search short-circuits; 条 5 全相关短路)
    //   - SELECTIVE + findRelevant empty/failed → seedActive(pool)
    //     (flag=true; 条 6 行 3 降级 — 退化 progressive)
    //   - PROGRESSIVE → seedActive(pool) (flag=true always; 条 4)
    // Same logic mirrored for skills side (seedSkillActive /
    // seedSkillActiveSubset / isActiveFullSkillPool).
    if (IsProgressiveDisclosureActive() && workerEnv_ != nullptr) {
        TurnState* ts = workerEnv_->GetCurrentTurnState();
        if (ts != nullptr) {
            ts->reset();
            // Snapshot tool pool under toolMutex_ (locks L5).
            std::vector<std::string> pool;
            {
                std::lock_guard<std::mutex> lock(toolMutex_);
                pool = toolNames_;
            }
            // Skill pool: SkillEngine is Agent-scoped; GetSkillIds() takes
            // its own mutex internally. Empty when no skills configured.
            std::vector<std::string> skillPool;
            if (skillEngine_ != nullptr) {
                skillPool = skillEngine_->GetSkillIds();
            }
            // V3 (round5 §5.4.2): read effectiveMode (lazily resolved from
            // AUTO if needed) instead of config_.toolDisclosureMode. AUTO may
            // resolve to SELECTIVE, in which case the findRelevant path
            // below must fire. Reading config_.toolDisclosureMode would
            // return AUTO (unresolved) and miss the SELECTIVE branch.
            // GetEffectiveMode() triggers IsProgressiveDisclosureActive()
            // for its side effect (call_once resolution), then returns
            // effectiveMode_.
            if (GetEffectiveMode() == ToolDisclosureMode::SELECTIVE
                && capabilitySelector_ != nullptr) {
                // V2: run findRelevant once at turn-start. sessionContext is
                // the windowed conversation history (条 7: reuse
                // ContextEngine::GetContextWindow — already window-limited).
                auto sessionContext = contextEngine != nullptr
                    ? contextEngine->GetContextWindow()
                    : std::vector<Message>{};
                CapabilitySelection rr;
                try {
                    rr = capabilitySelector_->findRelevant(query, sessionContext);
                } catch (const std::exception& e) {
                    LOG(ERR) << "[Invoke] findRelevant failed: " << e.what()
                             << "; falling back to full pool seed (退化 progressive).";
                    rr = {};
                }
                // Per-side seed decision (条 6 降级判定表, applied per-side).
                // Three distinct branches per side:
                //   行 3 降级 (empty)          → seedActive(pool)      flag=true  [+WARN]
                //   行 2 全相关短路 (== pool)   → seedActive(pool)      flag=true
                //   行 1 正常子集               → seedActiveSubset(rr)  flag=false
                // 行 4 "全在 alwaysOn" subsumed by 正常子集 branch (names ⊆ pool
                // but ≠ pool — findRelevant 返非空子集就走这里, even if all
                // returned names happen to be in alwaysOn, that's still a subset
                // of pool so search routes to real recall per 条 6 行 4).
                //
                // Splitting 降级 and 全相关 makes the per-side consequence
                // observable in logs; previously both collapsed into
                // seedActive(pool) silently. Root cause (exception / empty
                // response / parse failure) is already logged inside
                // CapabilitySelector (LOG ERR/WARN); the WARN here marks the
                // CONSEQUENCE (this side退化d to progressive behavior) so
                // operators can grep "SELECTIVE degraded" to find降级 events
                // without parsing root cause logs.
                std::set<std::string> toolsSet(rr.tools.begin(), rr.tools.end());
                std::set<std::string> poolSet(pool.begin(), pool.end());
                if (rr.tools.empty()) {
                    LOG(WARN) << "[Invoke] SELECTIVE degraded to PROGRESSIVE on tool side "
                              << "(findRelevant returned empty tool set; seeding full pool of "
                              << pool.size() << " tools). Root cause logged by CapabilitySelector.";
                    ts->seedActive(pool);  // flag=true → search short-circuits
                } else if (toolsSet == poolSet) {
                    // 行 2 全相关短路: subset happens to equal full pool —
                    // short-circuit (no WARN, this is a successful recall where
                    // everything happens to be relevant).
                    ts->seedActive(pool);  // flag=true
                } else {
                    // 行 1 正常子集 (条 6 行 4 "全在 alwaysOn" subsumed here:
                    // names ⊆ pool, ≠ pool).
                    ts->seedActiveSubset(rr.tools);  // flag=false → search routes to real recall
                }
                // Skills side (symmetric):
                std::set<std::string> skillsSet(rr.skills.begin(), rr.skills.end());
                std::set<std::string> skillPoolSet(skillPool.begin(), skillPool.end());
                if (rr.skills.empty()) {
                    LOG(WARN) << "[Invoke] SELECTIVE degraded to PROGRESSIVE on skill side "
                              << "(findRelevant returned empty skill set; seeding full pool of "
                              << skillPool.size() << " skills). Root cause logged by CapabilitySelector.";
                    ts->seedSkillActive(skillPool);  // flag=true
                } else if (skillsSet == skillPoolSet) {
                    ts->seedSkillActive(skillPool);  // flag=true
                } else {
                    ts->seedSkillActiveSubset(rr.skills);  // flag=false
                }
            } else {
                // PROGRESSIVE (or SELECTIVE without capabilitySelector_ —
                // shouldn't happen but be defensive): seed full pool on both
                // sides. Flag=true → search short-circuits for both.
                ts->seedActive(pool);
                ts->seedSkillActive(skillPool);
            }
        }
    }

    return ReactLoop(query, contextEngine, std::move(callback), myGeneration);
}

} // namespace jiuwen

#include "src/models/openai_model.h"

#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "src/models/retry_helper.h"
#include "src/utils/encoding.h"
#include "src/utils/logger.h"
#include "third_party/include/curl/curl.h"
#include "third_party/include/nlohmann/json.hpp"

using json = nlohmann::json;

namespace jiuwen {

namespace {

// Stream parser context. The OpenAI streaming protocol delivers:
//   * choices[0].delta.content  -- text fragments
//   * choices[0].delta.tool_calls[*] -- incremental tool-call assembly,
//     each entry has an "index" that identifies the slot; the first entry
//     for an index carries id+function.name, subsequent entries append to
//     function.arguments.
//   * choices[0].finish_reason -- terminal marker
struct StreamContext {
    std::function<void(const std::string&)> onChunk;
    std::string fullText;
    std::string buffer;
    std::string finishReason;
    bool finished{false};

    struct PartialToolCall {
        std::string id;
        std::string name;
        std::string argumentsJson;
    };
    // Ordered by the "index" field returned by the server.
    std::map<int, PartialToolCall> partialToolCalls;
};

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t totalSize = size * nmemb;
    auto* ctx = static_cast<StreamContext*>(userp);
    ctx->buffer.append(static_cast<char*>(contents), totalSize);

    while (true) {
        size_t pos = ctx->buffer.find('\n');
        if (pos == std::string::npos) break;
        std::string line = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 1);
        if (line.rfind("data: ", 0) != 0) continue;
        std::string dataStr = line.substr(6);
        if (dataStr == "[DONE]") {
            ctx->finished = true;
            continue;
        }
        try {
            auto data = json::parse(dataStr);
            if (!data.contains("choices") || !data["choices"].is_array() || data["choices"].empty()) {
                continue;
            }
            auto& choice = data["choices"][0];
            if (choice.contains("delta")) {
                const auto& delta = choice["delta"];
                if (delta.contains("content") && !delta["content"].is_null() && delta["content"].is_string()) {
                    std::string content = delta["content"].get<std::string>();
                    if (!content.empty()) {
                        ctx->fullText += content;
                        if (ctx->onChunk) ctx->onChunk(content);
                    }
                }
                if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                    for (const auto& tc : delta["tool_calls"]) {
                        int idx = tc.value("index", 0);
                        auto& slot = ctx->partialToolCalls[idx];
                        if (tc.contains("id") && tc["id"].is_string()) {
                            slot.id = tc["id"].get<std::string>();
                        }
                        if (tc.contains("function")) {
                            const auto& fn = tc["function"];
                            if (fn.contains("name") && fn["name"].is_string()) {
                                slot.name += fn["name"].get<std::string>();
                            }
                            if (fn.contains("arguments") && fn["arguments"].is_string()) {
                                slot.argumentsJson += fn["arguments"].get<std::string>();
                            }
                        }
                    }
                }
            }
            if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                ctx->finishReason = choice["finish_reason"].get<std::string>();
                ctx->finished = true;
            }
        } catch (const std::exception& e) {
            std::cerr << "[OpenAI] JSON parse error on line: " << e.what() << std::endl;
        }
    }
    return totalSize;
}

// Build the openai-style tools array from ToolSchema list.
json ToolsArrayJson(const std::vector<ToolSchema>& tools)
{
    json out = json::array();
    for (const auto& t : tools) {
        json entry;
        entry["type"] = "function";
        json fn;
        fn["name"] = t.name;
        fn["description"] = t.description;
        fn["parameters"] = t.parameters;
        entry["function"] = fn;
        out.push_back(entry);
    }
    return out;
}

// Helper used by the fallback (prompt-only) mode to dump every available
// tool into the system prompt so the model knows what it can call.
std::string ToolsAsTextSchema(const std::vector<ToolSchema>& tools)
{
    if (tools.empty()) return "";
    std::ostringstream oss;
    oss << "\n\n# Available tools\n";
    oss << "When you need to call a tool, output a JSON object exactly of the form:\n";
    oss << "  {\"name\": \"<tool>\", \"arguments\": { ... }}\n";
    oss << "Do not wrap it in code fences. After receiving the Observation, decide whether to call more tools or produce a final answer.\n\n";
    for (const auto& t : tools) {
        oss << "## " << t.name << "\n";
        oss << t.description << "\n";
        oss << "Parameters JSON Schema:\n" << t.parameters.dump(2) << "\n\n";
    }
    return oss.str();
}

struct ToolPairingStats
{
    int assistantToolCalls{0};
    int toolResults{0};
    int missingIds{0};
    int orphanToolResults{0};
    int orphanToolCalls{0};
};

ToolPairingStats ValidateToolPairing(const json& msgs)
{
    ToolPairingStats stats;
    std::unordered_set<std::string> callIds;
    std::unordered_set<std::string> resultIds;
    for (const auto& msg : msgs) {
        std::string role = msg.value("role", "");
        if (role == "assistant" && msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
            for (const auto& tc : msg["tool_calls"]) {
                ++stats.assistantToolCalls;
                std::string id = tc.value("id", "");
                if (id.empty()) {
                    ++stats.missingIds;
                } else {
                    callIds.insert(id);
                }
            }
        } else if (role == "tool") {
            ++stats.toolResults;
            std::string id = msg.value("tool_call_id", "");
            if (id.empty()) {
                ++stats.missingIds;
            } else {
                resultIds.insert(id);
            }
        }
    }
    for (const auto& id : callIds) {
        if (resultIds.find(id) == resultIds.end()) ++stats.orphanToolCalls;
    }
    for (const auto& id : resultIds) {
        if (callIds.find(id) == callIds.end()) ++stats.orphanToolResults;
    }
    return stats;
}

} // namespace

std::string OpenAIModel::Format(const std::string& systemPrompt,
                                 const std::vector<Message>& messages,
                                 const std::vector<ToolSchema>& tools)
{
    json payload;
    payload["model"] = config_.modelName;
    payload["stream"] = true;

    const bool useNative = config_.useNativeFunctionCalling;

    // Compose system prompt - in fallback mode we append the tool catalogue
    // so the model knows the signatures.
    std::string sys = systemPrompt;
    if (!useNative && !tools.empty()) {
        sys += ToolsAsTextSchema(tools);
    }

    json msgs = json::array();
    if (!sys.empty()) {
        msgs.push_back({{"role", "system"}, {"content", sys}});
    }

    for (const auto& m : messages) {
        std::string contentFixed = FixStringUTF8(m.content);
        if (m.role == "assistant") {
            if (useNative && !m.toolCalls.empty()) {
                json entry;
                entry["role"] = "assistant";
                entry["content"] = contentFixed;
                json arr = json::array();
                for (const auto& tc : m.toolCalls) {
                    json j;
                    j["id"] = tc.id;
                    j["type"] = "function";
                    j["function"]["name"] = tc.name;
                    j["function"]["arguments"] = tc.argumentsJson;
                    arr.push_back(j);
                }
                entry["tool_calls"] = arr;
                msgs.push_back(entry);
            } else if (!useNative && !m.toolCalls.empty()) {
                // Fallback: serialise tool_calls into content (single or first)
                std::string serialized;
                if (!contentFixed.empty()) {
                    serialized = contentFixed + "\n";
                }
                for (const auto& tc : m.toolCalls) {
                    json call;
                    call["name"] = tc.name;
                    try {
                        call["arguments"] = json::parse(tc.argumentsJson);
                    } catch (...) {
                        call["arguments"] = tc.argumentsJson;
                    }
                    serialized += call.dump() + "\n";
                }
                msgs.push_back({{"role", "assistant"}, {"content", serialized}});
            } else {
                msgs.push_back({{"role", "assistant"}, {"content", contentFixed}});
            }
            continue;
        }
        if (m.role == "tool") {
            if (useNative) {
                json entry;
                entry["role"] = "tool";
                entry["tool_call_id"] = m.toolCallId;
                entry["content"] = contentFixed;
                msgs.push_back(entry);
            } else {
                // Fallback: rewrite the tool result as a user turn so the
                // model has visibility without needing a real tool role.
                std::string observation = "Observation:\n" + contentFixed;
                if (!msgs.empty() && msgs.back().value("role", "") == "user") {
                    std::string prev = msgs.back().value("content", "");
                    msgs.back()["content"] = prev.empty() ? observation : prev + "\n\n" + observation;
                } else {
                    msgs.push_back({{"role", "user"}, {"content", observation}});
                }
            }
            continue;
        }
        // user / system / others pass through
        msgs.push_back({{"role", m.role}, {"content", contentFixed}});
    }
    payload["messages"] = msgs;

    if (useNative) {
        ToolPairingStats stats = ValidateToolPairing(msgs);
        LOG(INFO) << "[OpenAIModel] Tool pairing assistantToolCalls=" << stats.assistantToolCalls
                  << " toolResults=" << stats.toolResults
                  << " missingIds=" << stats.missingIds
                  << " orphanToolCalls=" << stats.orphanToolCalls
                  << " orphanToolResults=" << stats.orphanToolResults;
        if (stats.missingIds > 0 || stats.orphanToolCalls > 0 || stats.orphanToolResults > 0) {
            LOG(WARN) << "[OpenAIModel] Tool pairing validation found incompatible message structure";
        }
    }

    if (useNative && !tools.empty()) {
        payload["tools"] = ToolsArrayJson(tools);
    }

    // Inject common OpenAI-compatible request fields from extraParams. Only
    // keys explicitly provided by the caller are forwarded, so this is safe
    // across all OpenAI-compatible vendors (vLLM / ollama / DeepSeek / ARK /
    // Together / ...).
    const auto& ep = config_.extraParams;
    if (useNative && ep.GetPtr<bool>("parallel_tool_calls")) {
        payload["parallel_tool_calls"] = ep.GetValue<bool>("parallel_tool_calls", true);
    }
    if (ep.GetPtr<int>("max_tokens")) {
        payload["max_tokens"] = ep.GetValue<int>("max_tokens", 0);
    }
    if (ep.GetPtr<float>("temperature")) {
        payload["temperature"] = ep.GetValue<float>("temperature", 0.0f);
    }
    if (ep.GetPtr<float>("top_p")) {
        payload["top_p"] = ep.GetValue<float>("top_p", 0.0f);
    }
    if (ep.GetPtr<float>("presence_penalty")) {
        payload["presence_penalty"] = ep.GetValue<float>("presence_penalty", 0.0f);
    }
    if (ep.GetPtr<float>("frequency_penalty")) {
        payload["frequency_penalty"] = ep.GetValue<float>("frequency_penalty", 0.0f);
    }
    if (ep.GetPtr<int>("seed")) {
        payload["seed"] = ep.GetValue<int>("seed", 0);
    }
    return payload.dump();
}

ModelResponse OpenAIModel::DoInvokeOnce(const std::string& formattedInput,
                                          std::function<void(const std::string&)> onChunk)
{
    ModelResponse out;
    CURL* curl = curl_easy_init();
    if (!curl) {
        out.content = "Error: CURL init failed";
        out.finishReason = "error";
        out.isFinished = true;
        return out;
    }

    StreamContext ctx;
    ctx.onChunk = std::move(onChunk);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth = "Authorization: Bearer " + config_.apiKey;
    headers = curl_slist_append(headers, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, (config_.baseUrl + "/chat/completions").c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, formattedInput.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        out.content = std::string("Error: ") + curl_easy_strerror(res);
        out.finishReason = "error";
        out.isFinished = true;
        out.isRetryable = IsRetryableCurlError(res);
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        return out;
    }

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    out.statusCode = static_cast<int>(httpCode);

    if (httpCode >= 400) {
        out.content = ctx.fullText.empty()
            ? "Error: HTTP " + std::to_string(httpCode)
            : ctx.fullText;
        out.finishReason = "error";
        out.isFinished = true;
        out.isRetryable = IsRetryableHttpStatus(httpCode);
        return out;
    }

    out.content = ctx.fullText;
    out.finishReason = ctx.finishReason.empty() ? "stop" : ctx.finishReason;
    out.isFinished = true;

    if (config_.useNativeFunctionCalling) {
        for (auto& kv : ctx.partialToolCalls) {
            ToolCall tc;
            tc.id = kv.second.id;
            tc.name = kv.second.name;
            tc.argumentsJson = kv.second.argumentsJson.empty() ? "{}" : kv.second.argumentsJson;
            out.toolCalls.push_back(std::move(tc));
        }
    }
    return out;
}

ModelResponse OpenAIModel::Invoke(const std::string& formattedInput,
                                   std::function<void(const std::string&)> onChunk)
{
    const auto& policy = config_.retryPolicy;
    int totalAttempts = 1 + policy.maxRetries;

    for (int attempt = 0; attempt < totalAttempts; ++attempt) {
        bool isFinalAttempt = (attempt == totalAttempts - 1);

        ModelResponse out = DoInvokeOnce(formattedInput, isFinalAttempt ? onChunk : nullptr);

        if (!out.isRetryable) {
            return out;
        }

        if (isFinalAttempt) {
            LOG(WARN) << "[OpenAI] Final attempt still failed: "
                      << out.content
                      << " (httpCode=" << (out.statusCode ? std::to_string(*out.statusCode) : "none") << ")";
            return out;
        }

        std::string errDetail;
        if (out.statusCode) errDetail = "httpCode=" + std::to_string(*out.statusCode);
        else errDetail = "curlErr=" + out.content;

        LOG(INFO) << "[OpenAI] Retry attempt " << (attempt + 1) << "/" << policy.maxRetries
                  << " after " << ComputeBackoffDelayMs(attempt, policy) << "ms"
                  << " (" << errDetail << ")";

        SleepBackoff(attempt, policy);
    }

    ModelResponse out;
    out.content = "Error: retry loop exhausted";
    out.finishReason = "error";
    out.isFinished = true;
    return out;
}

} // namespace jiuwen

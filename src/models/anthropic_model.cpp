#include "src/models/anthropic_model.h"

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "src/utils/encoding.h"
#include "third_party/include/curl/curl.h"
#include "third_party/include/nlohmann/json.hpp"

using json = nlohmann::json;

namespace jiuwen {

namespace {

struct AnthropicStreamContext {
    std::function<void(const std::string&)> onChunk;
    std::string fullText;
    std::string buffer;
    std::string finishReason;

    // Anthropic streams tool_use as content_block_start with
    // {type:"tool_use", id, name, input:{}}, then content_block_delta
    // streams input_json_delta fragments, then content_block_stop.
    struct PartialToolUse {
        std::string id;
        std::string name;
        std::string inputJson; // accumulated via input_json_delta
    };
    std::vector<PartialToolUse> toolUses; // ordered by block index
    int currentToolIndex{-1};
};

size_t AnthropicWriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t totalSize = size * nmemb;
    auto* ctx = static_cast<AnthropicStreamContext*>(userp);
    ctx->buffer.append(static_cast<char*>(contents), totalSize);

    while (true) {
        size_t pos = ctx->buffer.find('\n');
        if (pos == std::string::npos) break;
        std::string line = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 1);

        if (line.rfind("data: ", 0) != 0) continue;
        std::string dataStr = line.substr(6);
        if (dataStr.empty()) continue;

        try {
            json data = json::parse(dataStr);
            std::string type = data.value("type", "");

            if (type == "content_block_start") {
                int index = data.value("index", -1);
                const auto& block = data["content_block"];
                std::string blockType = block.value("type", "");
                if (blockType == "tool_use") {
                    ctx->currentToolIndex = static_cast<int>(ctx->toolUses.size());
                    AnthropicStreamContext::PartialToolUse tu;
                    tu.id = block.value("id", "");
                    tu.name = block.value("name", "");
                    if (block.contains("input") && block["input"].is_object()) {
                        tu.inputJson = block["input"].dump();
                    }
                    ctx->toolUses.push_back(std::move(tu));
                } else {
                    ctx->currentToolIndex = -1;
                }
            } else if (type == "content_block_delta") {
                int index = data.value("index", -1);
                const auto& delta = data["delta"];
                std::string deltaType = delta.value("type", "");
                if (deltaType == "text_delta") {
                    std::string text = delta.value("text", "");
                    if (!text.empty()) {
                        ctx->fullText += text;
                        if (ctx->onChunk) ctx->onChunk(text);
                    }
                } else if (deltaType == "input_json_delta") {
                    std::string partial = delta.value("partial_json", "");
                    if (!partial.empty()) {
                        if (index >= 0 && index < static_cast<int>(ctx->toolUses.size())) {
                            ctx->toolUses[index].inputJson += partial;
                        }
                    }
                }
            } else if (type == "content_block_stop") {
                // No action needed; tool_use input is now complete.
                ctx->currentToolIndex = -1;
            } else if (type == "message_delta") {
                if (data.contains("delta") && data["delta"].contains("stop_reason")) {
                    ctx->finishReason = data["delta"]["stop_reason"].get<std::string>();
                }
            } else if (type == "message_stop") {
                ctx->buffer.clear();
                break;
            }
        } catch (const std::exception& e) {
            std::cerr << "[Anthropic] JSON parse error: " << e.what() << std::endl;
        }
    }
    return totalSize;
}

// Build the Anthropic-style tools array from ToolSchema list.
// Each entry has name, description, and input_schema.
json AnthropicToolsArray(const std::vector<ToolSchema>& tools)
{
    json out = json::array();
    for (const auto& t : tools) {
        json entry;
        entry["name"] = t.name;
        entry["description"] = t.description;
        entry["input_schema"] = t.parameters;
        out.push_back(entry);
    }
    return out;
}

std::string FallbackToolText(const std::vector<ToolSchema>& tools)
{
    if (tools.empty()) return "";
    std::ostringstream oss;
    oss << "\n\n# Available tools\n";
    oss << "When calling a tool, output a JSON object exactly:\n";
    oss << R"(  {"name":"<tool>","arguments":{...}})";
    oss << "\nDo NOT wrap in code fences.\n\n";
    for (const auto& t : tools) {
        oss << "## " << t.name << "\n" << t.description
            << "\nParameters:\n" << t.parameters.dump(2) << "\n\n";
    }
    return oss.str();
}

} // namespace

std::string AnthropicModel::Format(const std::string& systemPrompt,
                                    const std::vector<Message>& messages,
                                    const std::vector<ToolSchema>& tools)
{
    json payload;
    payload["model"] = config_.modelName;
    payload["stream"] = true;
    payload["max_tokens"] = 4096;

    const bool useNative = config_.useNativeFunctionCalling;

    // System: in fallback mode we embed tool schemas into the system prompt.
    std::string sys = systemPrompt;
    if (!useNative && !tools.empty()) {
        sys += FallbackToolText(tools);
    }
    payload["system"] = sys;

    json msgs = json::array();

    // Helper: push a single tool result into the pending multi-tool user block.
    // Anthropic requires tool_results to be packed into a single user message
    // with content:[{type:"tool_result",tool_use_id,content}].
    auto startToolResultBlock = [&]() {
        json u;
        u["role"] = "user";
        json contentArr = json::array();
        u["content"] = contentArr;
        msgs.push_back(u);
    };

    // We will fold consecutive role=tool messages into a single user content
    // block. The last message that was pushed determines where we add.
    bool awaitingToolBlock = false;

    auto appendToolResult = [&](const std::string& toolUseId, const std::string& content) {
        if (msgs.empty() || !awaitingToolBlock) {
            startToolResultBlock();
            awaitingToolBlock = true;
        }
        json tr;
        tr["type"] = "tool_result";
        tr["tool_use_id"] = toolUseId;
        tr["content"] = content;
        msgs.back()["content"].push_back(tr);
    };

    for (const auto& m : messages) {
        std::string fixed = FixStringUTF8(m.content);

        if (m.role == "assistant") {
            awaitingToolBlock = false;
            if (useNative && !m.toolCalls.empty()) {
                json entry;
                entry["role"] = "assistant";
                json contentArr = json::array();
                if (!fixed.empty()) {
                    json textBlock;
                    textBlock["type"] = "text";
                    textBlock["text"] = fixed;
                    contentArr.push_back(textBlock);
                }
                for (const auto& tc : m.toolCalls) {
                    json toolBlock;
                    toolBlock["type"] = "tool_use";
                    toolBlock["id"] = tc.id;
                    toolBlock["name"] = tc.name;
                    // input must be a JSON object, not a string
                    try {
                        toolBlock["input"] = json::parse(tc.argumentsJson);
                    } catch (...) {
                        toolBlock["input"] = json::object();
                    }
                    contentArr.push_back(toolBlock);
                }
                entry["content"] = contentArr;
                msgs.push_back(entry);
            } else if (!useNative && !m.toolCalls.empty()) {
                std::string serialized;
                if (!fixed.empty()) serialized = fixed + "\n";
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
                json entry;
                entry["role"] = "assistant";
                entry["content"] = {{"type", "text"}, {"text", serialized}};
                msgs.push_back(entry);
            } else {
                json entry;
                entry["role"] = "assistant";
                entry["content"] = {{"type", "text"}, {"text", fixed}};
                msgs.push_back(entry);
            }
            continue;
        }

        if (m.role == "tool") {
            if (useNative) {
                appendToolResult(m.toolCallId, fixed);
            } else {
                std::string observation = "Observation:\n" + fixed;
                if (!msgs.empty() && msgs.back()["role"] == "user") {
                    // Can't merge into user when the user msg is tool_result blocks
                    if (!awaitingToolBlock) {
                        std::string prev = msgs.back()["content"].is_string()
                            ? msgs.back()["content"].get<std::string>() : "";
                        msgs.back()["content"] = prev.empty() ? observation : prev + "\n\n" + observation;
                    } else {
                        // Current user message is a tool_result block;
                        // start a new user message for fallback
                        msgs.push_back({{"role","user"}, {"content", observation}});
                        awaitingToolBlock = false;
                    }
                } else {
                    msgs.push_back({{"role","user"}, {"content", observation}});
                }
            }
            continue;
        }

        // user / system / other
        if (m.role == "user" && awaitingToolBlock) {
            // A real user message after tool results: flush the tool block
            // state and emit as a separate message.
            awaitingToolBlock = false;
        }
        msgs.push_back({{"role", m.role}, {"content", fixed}});
    }

    payload["messages"] = msgs;

    if (useNative && !tools.empty()) {
        payload["tools"] = AnthropicToolsArray(tools);
    }

    return payload.dump();
}

ModelResponse AnthropicModel::Invoke(const std::string& formattedInput,
                                      std::function<void(const std::string&)> onChunk)
{
    ModelResponse out;
    CURL* curl = curl_easy_init();
    if (!curl) {
        out.content = "Error: CURL init failed";
        return out;
    }

    AnthropicStreamContext ctx;
    ctx.onChunk = std::move(onChunk);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("x-api-key: " + config_.apiKey).c_str());
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

    curl_easy_setopt(curl, CURLOPT_URL, (config_.baseUrl + "/v1/messages").c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, formattedInput.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AnthropicWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        out.content = std::string("Error: ") + curl_easy_strerror(res);
        out.finishReason = "error";
        out.isFinished = true;
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        return out;
    }
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    out.content = ctx.fullText;
    // Anthropic maps: end_turn → stop, tool_use → tool_calls, max_tokens → length
    std::string fr = ctx.finishReason;
    if (fr == "end_turn") fr = "stop";
    else if (fr == "tool_use") fr = "tool_calls";
    else if (fr == "max_tokens") fr = "length";
    out.finishReason = fr;
    out.isFinished = true;

    if (config_.useNativeFunctionCalling) {
        for (const auto& tu : ctx.toolUses) {
            ToolCall tc;
            tc.id = tu.id;
            tc.name = tu.name;
            tc.argumentsJson = tu.inputJson.empty() ? "{}" : tu.inputJson;
            // Ensure it is valid JSON.
            try {
                json::parse(tc.argumentsJson);
            } catch (...) {
                tc.argumentsJson = "{}";
            }
            out.toolCalls.push_back(std::move(tc));
        }
    }
    return out;
}

} // namespace jiuwen
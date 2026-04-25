#include "src/models/anthropic_model.h"
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "third_party/include/curl/curl.h"
#include "third_party/include/nlohmann/json.hpp"

using json = nlohmann::json;

namespace jiuwen {

struct AnthropicStreamContext {
    std::function<void(const std::string&)> onChunk;
    std::string fullResponse;
    std::string buffer;
};

static size_t AnthropicWriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t totalSize = size * nmemb;
    auto* ctx = static_cast<AnthropicStreamContext*>(userp);
    ctx->buffer.append(static_cast<char*>(contents), totalSize);

    while (true) {
        size_t pos = ctx->buffer.find("\n");
        if (pos == std::string::npos) break;

        std::string line = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 1);

        if (line.rfind("data: ", 0) == 0) {
            std::string dataStr = line.substr(6);
            if (dataStr.empty()) continue;

            try {
                json data = json::parse(dataStr);
                std::string type = data.value("type", "");

                if (type == "content_block_delta") {
                    if (data.contains("delta") && data["delta"].contains("text")) {
                        std::string text = data["delta"]["text"].get<std::string>();
                        if (!text.empty()) {
                            ctx->fullResponse += text;
                            if (ctx->onChunk) ctx->onChunk(text);
                        }
                    }
                } else if (type == "message_stop") {
                    ctx->buffer.clear();
                    break;
                }
            } catch (const std::exception& e) {
                std::cerr << "[Anthropic] JSON Parse Error: " << e.what() << std::endl;
            }
        }
    }
    return totalSize;
}

std::string AnthropicModel::Format(const std::string& systemPrompt, const std::vector<Message>& messages)
{
    json payload;
    payload["model"] = config_.modelName;
    payload["stream"] = true;
    payload["max_tokens"] = 4096;
    payload["system"] = systemPrompt;

    json msgs = json::array();
    for (const auto& msg : messages) {
        msgs.push_back({
            {"role", msg.role},
            {"content", {{"type", "text"}, {"text", msg.content}}}
        });
    }
    payload["messages"] = msgs;

    return payload.dump();
}

std::string AnthropicModel::Invoke(const std::string& formattedInput, std::function<void(const std::string&)> onChunk)
{
    CURL* curl = curl_easy_init();
    if (!curl) return "Error: CURL init failed";

    AnthropicStreamContext ctx;
    ctx.onChunk = onChunk;

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
    std::string result = ctx.fullResponse;
    if (res != CURLE_OK) {
        result = "Error: " + std::string(curl_easy_strerror(res));
    }

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return result;
}

ModelResponse AnthropicModel::ParseResponse(const std::string& rawResponse)
{
    return {rawResponse, true, "end_turn"};
}

} // namespace jiuwen

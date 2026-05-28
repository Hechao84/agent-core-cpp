#include "examples/jiuwenClaw/models/ark_code_model.h"
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "src/utils/encoding.h"
#include "third_party/include/curl/curl.h"
#include "third_party/include/nlohmann/json.hpp"

using json = nlohmann::json;

namespace {

struct StreamContext {
    std::function<void(const std::string&)> onChunk;
    std::string fullResponse;
    std::string buffer;
};

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t totalSize = size * nmemb;
    auto* ctx = static_cast<StreamContext*>(userp);
    ctx->buffer.append(static_cast<char*>(contents), totalSize);

    while (true) {
        size_t pos = ctx->buffer.find("\n");
        if (pos == std::string::npos) break;

        std::string line = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 1);

        if (line.rfind("data: ", 0) == 0) {
            std::string dataStr = line.substr(6);
            if (dataStr == "[DONE]") continue;

            try {
                json data = json::parse(dataStr);
                if (data.contains("choices") && data["choices"].is_array() && !data["choices"].empty()) {
                    auto& choice = data["choices"][0];
                    if (choice.contains("delta") && choice["delta"].contains("content")) {
                        const auto& contentJson = choice["delta"]["content"];
                        if (!contentJson.is_null() && contentJson.is_string()) {
                            std::string content = contentJson.get<std::string>();
                            if (!content.empty()) {
                                ctx->fullResponse += content;
                                if (ctx->onChunk) ctx->onChunk(content);
                            }
                        }
                    }
                    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                        ctx->buffer.clear();
                        break;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[ArkCode] JSON Parse Error: " << e.what() << std::endl;
            }
        }
    }
    return totalSize;
}

} // namespace

namespace jiuwen {

ArkCodeModel::ArkCodeModel(ModelConfig config) : Model(std::move(config)) {}

std::string ArkCodeModel::Format(const std::string& systemPrompt, const std::vector<Message>& messages)
{
    json payload;
    payload["model"] = config_.modelName;
    payload["stream"] = true;

    json msgs = json::array();
    if (!systemPrompt.empty()) {
        msgs.push_back({{"role", "system"}, {"content", systemPrompt}});
    }

    for (const auto& msg : messages) {
        if (msg.role == "tool") {
            std::string content = "[Tool Result]\n" + FixStringUTF8(msg.content);
            msgs.push_back({{"role", "user"}, {"content", content}});
        } else {
            msgs.push_back({{"role", msg.role}, {"content", FixStringUTF8(msg.content)}});
        }
    }
    payload["messages"] = msgs;

    if (config_.extraParams.GetPtr<int>("max_tokens")) {
        payload["max_tokens"] = config_.extraParams.GetValue<int>("max_tokens", 4096);
    }
    if (config_.extraParams.GetPtr<float>("temperature")) {
        payload["temperature"] = config_.extraParams.GetValue<float>("temperature", 0.0f);
    }
    if (config_.extraParams.GetPtr<float>("top_p")) {
        payload["top_p"] = config_.extraParams.GetValue<float>("top_p", 0.0f);
    }

    return payload.dump();
}

std::string ArkCodeModel::Invoke(const std::string& formattedInput, std::function<void(const std::string&)> onChunk)
{
    CURL* curl = curl_easy_init();
    if (!curl) return "Error: CURL init failed";

    StreamContext ctx;
    ctx.onChunk = onChunk;

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
    std::string result = ctx.fullResponse;
    if (res != CURLE_OK) {
        result = "Error: " + std::string(curl_easy_strerror(res));
    }

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return result;
}

ModelResponse ArkCodeModel::ParseResponse(const std::string& rawResponse)
{
    return {rawResponse, true, "stop"};
}

} // namespace jiuwen

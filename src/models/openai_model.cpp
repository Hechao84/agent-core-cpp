#include "models/openai_model.h"
#include <curl/curl.h>
#include <iostream>
#include <sstream>
#include <string>
#include "nlohmann/json.hpp"

using json = nlohmann::json;

struct StreamContext {
    std::function<void(const std::string&)> onChunk;
    std::string fullResponse;
    std::string buffer;
};

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
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
                        // Stream finished
                        ctx->buffer.clear(); 
                        break;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[OpenAI] JSON Parse Error: " << e.what() << std::endl;
            }
        }
    }
    return totalSize;
}

std::string OpenAIModel::Format(const std::string& systemPrompt, const std::vector<Message>& messages) {
    json payload;
    payload["model"] = m_config.modelName;
    payload["stream"] = true;
    
    json msgs = json::array();
    if (!systemPrompt.empty()) {
        msgs.push_back({{"role", "system"}, {"content", systemPrompt}});
    }
    for (const auto& msg : messages) {
        msgs.push_back({{"role", msg.role}, {"content", msg.content}});
    }
    payload["messages"] = msgs;
    
    return payload.dump();
}

std::string OpenAIModel::Invoke(const std::string& formattedInput, std::function<void(const std::string&)> onChunk) {
    CURL* curl = curl_easy_init();
    if (!curl) return "Error: CURL init failed";

    StreamContext ctx;
    ctx.onChunk = onChunk;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth = "Authorization: Bearer " + m_config.apiKey;
    headers = curl_slist_append(headers, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, (m_config.baseUrl + "/chat/completions").c_str());
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

ModelResponse OpenAIModel::ParseResponse(const std::string& rawResponse) {
    return {rawResponse, true, "stop"};
}

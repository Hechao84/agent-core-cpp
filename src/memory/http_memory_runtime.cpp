#include "src/memory/http_memory_runtime.h"

#include <atomic>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <utility>

#include "src/utils/logger.h"
#include "src/utils/retry_helper.h"

namespace jiuwen {

namespace {

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output)
{
    size_t totalSize = size * nmemb;
    output->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

std::string UrlEncode(const std::string& value)
{
    CURL* curl = curl_easy_init();
    if (!curl) { return value; }
    char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    std::string result = encoded ? encoded : value;
    curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

bool IsSuccessEnvelope(const nlohmann::json& j)
{
    return j.contains("ok") && j["ok"].is_boolean() && j["ok"].get<bool>();
}

std::string ExtractError(const nlohmann::json& j)
{
    if (j.contains("error")) {
        if (j["error"].is_string()) { return j["error"].get<std::string>(); }
        if (j["error"].is_object() && j["error"].contains("message")) {
            return j["error"]["message"].get<std::string>();
        }
    }
    return "unknown error";
}

MemoryEventType EventTypeFromString(const std::string& s)
{
    if (s == "session_started") return MemoryEventType::SESSION_STARTED;
    if (s == "session_ended") return MemoryEventType::SESSION_ENDED;
    if (s == "message_appended") return MemoryEventType::MESSAGE_APPENDED;
    if (s == "tool_call_started") return MemoryEventType::TOOL_CALL_STARTED;
    if (s == "tool_call_finished") return MemoryEventType::TOOL_CALL_FINISHED;
    if (s == "payload_offloaded") return MemoryEventType::PAYLOAD_OFFLOADED;
    if (s == "consolidation_requested") return MemoryEventType::CONSOLIDATION_REQUESTED;
    if (s == "consolidation_completed") return MemoryEventType::CONSOLIDATION_COMPLETED;
    return MemoryEventType::MESSAGE_APPENDED;
}

std::string EventTypeToString(MemoryEventType type)
{
    switch (type) {
        case MemoryEventType::SESSION_STARTED: return "session_started";
        case MemoryEventType::SESSION_ENDED: return "session_ended";
        case MemoryEventType::MESSAGE_APPENDED: return "message_appended";
        case MemoryEventType::TOOL_CALL_STARTED: return "tool_call_started";
        case MemoryEventType::TOOL_CALL_FINISHED: return "tool_call_finished";
        case MemoryEventType::PAYLOAD_OFFLOADED: return "payload_offloaded";
        case MemoryEventType::CONSOLIDATION_REQUESTED: return "consolidation_requested";
        case MemoryEventType::CONSOLIDATION_COMPLETED: return "consolidation_completed";
    }
    return "message_appended";
}

} // namespace

HttpMemoryRuntime::HttpMemoryRuntime(MemoryConfig config)
    : MemoryRuntime(std::move(config)),
      serverUrl_(config_.serverUrl),
      apiKey_(config_.serverApiKey),
      timeoutSeconds_(config_.serverTimeoutSeconds),
      maxRetries_(config_.serverMaxRetries),
      circuitThreshold_(config_.serverCircuitThreshold),
      circuitCooldownMs_(config_.serverCircuitCooldownSeconds * 1000)
{
    if (!serverUrl_.empty() && serverUrl_.back() == '/') {
        serverUrl_.pop_back();
    }
}

HttpMemoryRuntime::~HttpMemoryRuntime() = default;

HttpMemoryRuntime::HttpResponse HttpMemoryRuntime::DoHttpPostOnce(const std::string& path, const std::string& jsonBody) const
{
    HttpResponse response{0, ""};
    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG(WARN) << "[HttpMemoryRuntime] Failed to initialize curl";
        return response;
    }

    std::string url = serverUrl_ + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jsonBody.size()));

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    if (!apiKey_.empty()) {
        std::string authHeader = "Authorization: Bearer " + apiKey_;
        headers = curl_slist_append(headers, authHeader.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeoutSeconds_);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds_);

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        LOG(WARN) << "[HttpMemoryRuntime] CURL error for " << path << ": " << curl_easy_strerror(result);
        response.isRetryable = IsRetryableCurlError(result);
        response.isCurlError = true;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    if (response.status >= 400 && !response.isCurlError) {
        response.isRetryable = IsRetryableHttpStatus(response.status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

HttpMemoryRuntime::HttpResponse HttpMemoryRuntime::HttpPost(const std::string& path, const std::string& jsonBody) const
{
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (circuit_.openUntilMs.load() > nowMs) {
        LOG(WARN) << "[HttpMemoryRuntime] Circuit open, skipping POST " << path;
        return {0, ""};
    }

    RetryPolicy policy;
    policy.maxRetries = maxRetries_;
    policy.baseDelayMs = 400;
    policy.maxDelayMs = 3000;
    policy.withJitter = true;

    int totalAttempts = 1 + policy.maxRetries;
    for (int attempt = 0; attempt < totalAttempts; ++attempt) {
        HttpResponse resp = DoHttpPostOnce(path, jsonBody);

        if (resp.status >= 200 && resp.status < 400 && !resp.isCurlError) {
            circuit_.consecutiveFailures.store(0);
            return resp;
        }

        if (!resp.isRetryable) {
            ++circuit_.consecutiveFailures;
            if (circuit_.consecutiveFailures.load() >= circuitThreshold_) {
                auto cooldownMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() + circuitCooldownMs_;
                circuit_.openUntilMs.store(cooldownMs);
                LOG(WARN) << "[HttpMemoryRuntime] Circuit opened after " << circuitThreshold_
                          << " consecutive failures, cooldown " << (circuitCooldownMs_ / 1000) << "s";
            }
            return resp;
        }

        if (attempt == totalAttempts - 1) {
            ++circuit_.consecutiveFailures;
            if (circuit_.consecutiveFailures.load() >= circuitThreshold_) {
                auto cooldownMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() + circuitCooldownMs_;
                circuit_.openUntilMs.store(cooldownMs);
                LOG(WARN) << "[HttpMemoryRuntime] Circuit opened, final retry still failed";
            }
            LOG(WARN) << "[HttpMemoryRuntime] POST " << path << " final attempt still failed"
                      << " (httpCode=" << resp.status << ")";
            return resp;
        }

        LOG(INFO) << "[HttpMemoryRuntime] POST " << path << " retry attempt " << (attempt + 1)
                  << "/" << policy.maxRetries << " after " << ComputeBackoffDelayMs(attempt, policy) << "ms"
                  << " (httpCode=" << resp.status << ")";
        SleepBackoff(attempt, policy);
    }

    return {0, ""};
}

HttpMemoryRuntime::HttpResponse HttpMemoryRuntime::DoHttpGetOnce(const std::string& path) const
{
    HttpResponse response{0, ""};
    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG(WARN) << "[HttpMemoryRuntime] Failed to initialize curl";
        return response;
    }

    std::string url = serverUrl_ + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    if (!apiKey_.empty()) {
        std::string authHeader = "Authorization: Bearer " + apiKey_;
        headers = curl_slist_append(headers, authHeader.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeoutSeconds_);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds_);

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        LOG(WARN) << "[HttpMemoryRuntime] CURL error for " << path << ": " << curl_easy_strerror(result);
        response.isRetryable = IsRetryableCurlError(result);
        response.isCurlError = true;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    if (response.status >= 400 && !response.isCurlError) {
        response.isRetryable = IsRetryableHttpStatus(response.status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

HttpMemoryRuntime::HttpResponse HttpMemoryRuntime::HttpGet(const std::string& path) const
{
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (circuit_.openUntilMs.load() > nowMs) {
        LOG(WARN) << "[HttpMemoryRuntime] Circuit open, skipping GET " << path;
        return {0, ""};
    }

    RetryPolicy policy;
    policy.maxRetries = maxRetries_;
    policy.baseDelayMs = 400;
    policy.maxDelayMs = 3000;
    policy.withJitter = true;

    int totalAttempts = 1 + policy.maxRetries;
    for (int attempt = 0; attempt < totalAttempts; ++attempt) {
        HttpResponse resp = DoHttpGetOnce(path);

        if (resp.status >= 200 && resp.status < 400 && !resp.isCurlError) {
            circuit_.consecutiveFailures.store(0);
            return resp;
        }

        if (!resp.isRetryable) {
            ++circuit_.consecutiveFailures;
            if (circuit_.consecutiveFailures.load() >= circuitThreshold_) {
                auto cooldownMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() + circuitCooldownMs_;
                circuit_.openUntilMs.store(cooldownMs);
                LOG(WARN) << "[HttpMemoryRuntime] Circuit opened after " << circuitThreshold_
                          << " consecutive failures, cooldown " << (circuitCooldownMs_ / 1000) << "s";
            }
            return resp;
        }

        if (attempt == totalAttempts - 1) {
            ++circuit_.consecutiveFailures;
            if (circuit_.consecutiveFailures.load() >= circuitThreshold_) {
                auto cooldownMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() + circuitCooldownMs_;
                circuit_.openUntilMs.store(cooldownMs);
            }
            LOG(WARN) << "[HttpMemoryRuntime] GET " << path << " final attempt still failed"
                      << " (httpCode=" << resp.status << ")";
            return resp;
        }

        LOG(INFO) << "[HttpMemoryRuntime] GET " << path << " retry attempt " << (attempt + 1)
                  << "/" << policy.maxRetries << " after " << ComputeBackoffDelayMs(attempt, policy) << "ms"
                  << " (httpCode=" << resp.status << ")";
        SleepBackoff(attempt, policy);
    }

    return {0, ""};
}

nlohmann::json HttpMemoryRuntime::SerializeEvent(const MemoryEvent& event) const
{
    nlohmann::json j;
    j["type"] = EventTypeToString(event.type);
    j["agentId"] = event.agentId;
    j["sessionId"] = event.sessionId;
    j["role"] = event.role;
    j["content"] = event.content;
    j["toolCallId"] = event.toolCallId;
    j["toolName"] = event.toolName;
    j["payloadRef"] = event.payloadRef;
    j["storeCursor"] = event.storeCursor;
    j["metadata"] = event.metadata;
    j["timestamp"] = event.timestamp;
    return j;
}

nlohmann::json HttpMemoryRuntime::SerializeContextRequest(const MemoryContextRequest& request) const
{
    nlohmann::json j;
    j["agentId"] = request.agentId;
    j["sessionId"] = request.sessionId;
    j["query"] = request.query;
    j["tokenBudget"] = request.tokenBudget;
    j["includeSections"] = request.includeSections;
    j["metadata"] = request.metadata;
    return j;
}

nlohmann::json HttpMemoryRuntime::SerializePayloadWriteRequest(const MemoryPayloadWriteRequest& request) const
{
    nlohmann::json j;
    j["agentId"] = request.agentId;
    j["sessionId"] = request.sessionId;
    j["content"] = request.content;
    j["contentType"] = request.contentType;
    j["toolCallId"] = request.toolCallId;
    j["toolName"] = request.toolName;
    j["metadata"] = request.metadata;
    return j;
}

nlohmann::json HttpMemoryRuntime::SerializeConsolidationRequest(const MemoryConsolidationRequest& request) const
{
    nlohmann::json j;
    j["agentId"] = request.agentId;
    j["sessionId"] = request.sessionId;
    j["maxEvents"] = request.maxEvents;
    j["forceReprocess"] = request.forceReprocess;
    j["metadata"] = request.metadata;
    return j;
}

nlohmann::json HttpMemoryRuntime::SerializeSearchRequest(const MemorySearchRequest& request) const
{
    nlohmann::json j;
    j["agentId"] = request.agentId;
    j["sessionId"] = request.sessionId;
    j["query"] = request.query;
    j["limit"] = request.limit;
    j["includeSections"] = request.includeSections;
    j["metadata"] = request.metadata;
    return j;
}

MemoryContextPackage HttpMemoryRuntime::DeserializeContextPackage(const nlohmann::json& j) const
{
    MemoryContextPackage pkg;
    if (!j.contains("context")) { return pkg; }
    const auto& c = j["context"];
    if (c.contains("messages") && c["messages"].is_array()) {
        for (const auto& m : c["messages"]) {
            MemoryMessage msg;
            if (m.contains("role")) msg.role = m["role"].get<std::string>();
            if (m.contains("content")) msg.content = m["content"].get<std::string>();
            if (m.contains("toolCallId")) msg.toolCallId = m["toolCallId"].get<std::string>();
            if (m.contains("toolName")) msg.toolName = m["toolName"].get<std::string>();
            if (m.contains("payloadRef")) msg.payloadRef = m["payloadRef"].get<std::string>();
            pkg.messages.push_back(std::move(msg));
        }
    }
    if (c.contains("memoryText")) pkg.memoryText = c["memoryText"].get<std::string>();
    if (c.contains("entities") && c["entities"].is_array()) {
        for (const auto& e : c["entities"]) {
            MemoryEntity entity;
            if (e.contains("id")) entity.id = e["id"].get<std::string>();
            if (e.contains("agentId")) entity.agentId = e["agentId"].get<std::string>();
            if (e.contains("entityType")) entity.entityType = e["entityType"].get<std::string>();
            if (e.contains("name")) entity.name = e["name"].get<std::string>();
            if (e.contains("summary")) entity.summary = e["summary"].get<std::string>();
            if (e.contains("confidence")) entity.confidence = e["confidence"].get<float>();
            if (e.contains("isActive")) entity.isActive = e["isActive"].get<bool>();
            if (e.contains("supersededByEntityId")) entity.supersededByEntityId = e["supersededByEntityId"].get<std::string>();
            if (e.contains("supersededEntityId")) entity.supersededEntityId = e["supersededEntityId"].get<std::string>();
            if (e.contains("sourceRefs") && e["sourceRefs"].is_array()) {
                for (const auto& s : e["sourceRefs"]) entity.sourceRefs.push_back(s.get<std::string>());
            }
            if (e.contains("metadata")) entity.metadata = e["metadata"];
            if (e.contains("createdAt")) entity.createdAt = e["createdAt"].get<std::string>();
            if (e.contains("updatedAt")) entity.updatedAt = e["updatedAt"].get<std::string>();
            pkg.entities.push_back(std::move(entity));
        }
    }
    if (c.contains("relations") && c["relations"].is_array()) {
        for (const auto& r : c["relations"]) {
            MemoryRelation rel;
            if (r.contains("id")) rel.id = r["id"].get<std::string>();
            if (r.contains("agentId")) rel.agentId = r["agentId"].get<std::string>();
            if (r.contains("fromEntityId")) rel.fromEntityId = r["fromEntityId"].get<std::string>();
            if (r.contains("relationType")) rel.relationType = r["relationType"].get<std::string>();
            if (r.contains("toEntityId")) rel.toEntityId = r["toEntityId"].get<std::string>();
            if (r.contains("confidence")) rel.confidence = r["confidence"].get<float>();
            if (r.contains("sourceRefs") && r["sourceRefs"].is_array()) {
                for (const auto& s : r["sourceRefs"]) rel.sourceRefs.push_back(s.get<std::string>());
            }
            if (r.contains("metadata")) rel.metadata = r["metadata"];
            if (r.contains("createdAt")) rel.createdAt = r["createdAt"].get<std::string>();
            if (r.contains("updatedAt")) rel.updatedAt = r["updatedAt"].get<std::string>();
            pkg.relations.push_back(std::move(rel));
        }
    }
    if (c.contains("payloadRefs") && c["payloadRefs"].is_array()) {
        for (const auto& p : c["payloadRefs"]) {
            MemoryPayloadRef pref;
            if (p.contains("agentId")) pref.agentId = p["agentId"].get<std::string>();
            if (p.contains("sessionId")) pref.sessionId = p["sessionId"].get<std::string>();
            if (p.contains("uri")) pref.uri = p["uri"].get<std::string>();
            if (p.contains("contentType")) pref.contentType = p["contentType"].get<std::string>();
            if (p.contains("summary")) pref.summary = p["summary"].get<std::string>();
            if (p.contains("toolName")) pref.toolName = p["toolName"].get<std::string>();
            if (p.contains("originalChars")) pref.originalChars = p["originalChars"].get<int>();
            if (p.contains("metadata")) pref.metadata = p["metadata"];
            if (p.contains("createdAt")) pref.createdAt = p["createdAt"].get<std::string>();
            pkg.payloadRefs.push_back(std::move(pref));
        }
    }
    if (c.contains("citations") && c["citations"].is_array()) {
        for (const auto& s : c["citations"]) pkg.citations.push_back(s.get<std::string>());
    }
    if (c.contains("metadata")) pkg.metadata = c["metadata"];
    return pkg;
}

MemoryPayloadWriteResult HttpMemoryRuntime::DeserializePayloadWriteResult(const nlohmann::json& j) const
{
    MemoryPayloadWriteResult result;
    if (j.contains("succeeded")) result.succeeded = j["succeeded"].get<bool>();
    if (j.contains("offloaded")) result.offloaded = j["offloaded"].get<bool>();
    if (j.contains("replacementContent")) result.replacementContent = j["replacementContent"].get<std::string>();
    if (j.contains("payload") && j["payload"].is_object()) {
        const auto& p = j["payload"];
        if (p.contains("agentId")) result.payload.agentId = p["agentId"].get<std::string>();
        if (p.contains("sessionId")) result.payload.sessionId = p["sessionId"].get<std::string>();
        if (p.contains("uri")) result.payload.uri = p["uri"].get<std::string>();
        if (p.contains("contentType")) result.payload.contentType = p["contentType"].get<std::string>();
        if (p.contains("summary")) result.payload.summary = p["summary"].get<std::string>();
        if (p.contains("toolName")) result.payload.toolName = p["toolName"].get<std::string>();
        if (p.contains("originalChars")) result.payload.originalChars = p["originalChars"].get<int>();
        if (p.contains("metadata")) result.payload.metadata = p["metadata"];
        if (p.contains("createdAt")) result.payload.createdAt = p["createdAt"].get<std::string>();
    }
    return result;
}

std::vector<MemorySearchHit> HttpMemoryRuntime::DeserializeSearchHits(const nlohmann::json& j) const
{
    std::vector<MemorySearchHit> hits;
    if (!j.contains("hits") || !j["hits"].is_array()) { return hits; }
    for (const auto& h : j["hits"]) {
        MemorySearchHit hit;
        if (h.contains("id")) hit.id = h["id"].get<std::string>();
        if (h.contains("type")) hit.type = h["type"].get<std::string>();
        if (h.contains("content")) hit.content = h["content"].get<std::string>();
        if (h.contains("score")) hit.score = h["score"].get<float>();
        if (h.contains("sourceRefs") && h["sourceRefs"].is_array()) {
            for (const auto& s : h["sourceRefs"]) hit.sourceRefs.push_back(s.get<std::string>());
        }
        if (h.contains("metadata")) hit.metadata = h["metadata"];
        hits.push_back(std::move(hit));
    }
    return hits;
}

MemoryStats HttpMemoryRuntime::DeserializeStats(const nlohmann::json& j) const
{
    MemoryStats stats;
    if (!j.contains("stats")) { return stats; }
    const auto& s = j["stats"];
    if (s.contains("events")) stats.events = s["events"].get<int>();
    if (s.contains("payloads")) stats.payloads = s["payloads"].get<int>();
    if (s.contains("summaries")) stats.summaries = s["summaries"].get<int>();
    if (s.contains("entities")) stats.entities = s["entities"].get<int>();
    if (s.contains("relations")) stats.relations = s["relations"].get<int>();
    if (s.contains("metadata")) stats.metadata = s["metadata"];
    return stats;
}

bool HttpMemoryRuntime::AppendEvent(const MemoryEvent& event)
{
    nlohmann::json body = SerializeEvent(event);
    auto resp = HttpPost("/v1/events", body.dump());
    if (resp.status != 200) {
        LOG(WARN) << "[HttpMemoryRuntime] AppendEvent HTTP " << resp.status << ": " << resp.body;
        ++circuit_.appendFailures;
        return false;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(resp.body);
        if (!IsSuccessEnvelope(j)) {
            LOG(WARN) << "[HttpMemoryRuntime] AppendEvent failed: " << ExtractError(j);
            ++circuit_.appendFailures;
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        LOG(WARN) << "[HttpMemoryRuntime] AppendEvent parse error: " << e.what();
        ++circuit_.appendFailures;
        return false;
    }
}

MemoryContextPackage HttpMemoryRuntime::BuildContext(const MemoryContextRequest& request)
{
    nlohmann::json body = SerializeContextRequest(request);
    auto resp = HttpPost("/v1/context", body.dump());
    if (resp.status != 200) {
        LOG(WARN) << "[HttpMemoryRuntime] BuildContext HTTP " << resp.status << ": " << resp.body;
        ++circuit_.buildContextFailures;
        return {};
    }
    try {
        nlohmann::json j = nlohmann::json::parse(resp.body);
        if (!IsSuccessEnvelope(j)) {
            LOG(WARN) << "[HttpMemoryRuntime] BuildContext failed: " << ExtractError(j);
            ++circuit_.buildContextFailures;
            return {};
        }
        return DeserializeContextPackage(j);
    } catch (const std::exception& e) {
        LOG(WARN) << "[HttpMemoryRuntime] BuildContext parse error: " << e.what();
        ++circuit_.buildContextFailures;
        return {};
    }
}

MemoryPayloadWriteResult HttpMemoryRuntime::WritePayload(const MemoryPayloadWriteRequest& request)
{
    nlohmann::json body = SerializePayloadWriteRequest(request);
    auto resp = HttpPost("/v1/payloads", body.dump());
    MemoryPayloadWriteResult result;
    if (resp.status != 200) {
        LOG(WARN) << "[HttpMemoryRuntime] WritePayload HTTP " << resp.status << ": " << resp.body;
        ++circuit_.writeFailures;
        return result;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(resp.body);
        if (!IsSuccessEnvelope(j)) {
            LOG(WARN) << "[HttpMemoryRuntime] WritePayload failed: " << ExtractError(j);
            ++circuit_.writeFailures;
            return result;
        }
        return DeserializePayloadWriteResult(j);
    } catch (const std::exception& e) {
        LOG(WARN) << "[HttpMemoryRuntime] WritePayload parse error: " << e.what();
        ++circuit_.writeFailures;
        return result;
    }
}

std::string HttpMemoryRuntime::ReadPayload(const std::string& uri)
{
    std::string pathPart = uri;
    if (pathPart.find("file://") == 0) {
        pathPart = pathPart.substr(7);
    }
    std::string encodedPath = UrlEncode(pathPart);
    auto resp = HttpGet("/v1/payloads/" + encodedPath);
    if (resp.status != 200) {
        LOG(WARN) << "[HttpMemoryRuntime] ReadPayload HTTP " << resp.status << ": " << resp.body;
        return "";
    }
    try {
        nlohmann::json j = nlohmann::json::parse(resp.body);
        if (!IsSuccessEnvelope(j)) {
            LOG(WARN) << "[HttpMemoryRuntime] ReadPayload failed: " << ExtractError(j);
            return "";
        }
        if (j.contains("content")) {
            return j["content"].get<std::string>();
        }
        return "";
    } catch (const std::exception& e) {
        LOG(WARN) << "[HttpMemoryRuntime] ReadPayload parse error: " << e.what();
        return "";
    }
}

bool HttpMemoryRuntime::Consolidate(const MemoryConsolidationRequest& request, MemoryModelClient* modelClient)
{
    if (modelClient) {
        LOG(WARN) << "[HttpMemoryRuntime] modelClient is not used in server mode. "
                  << "Remote server uses its own model.";
    }
    nlohmann::json body = SerializeConsolidationRequest(request);
    auto resp = HttpPost("/v1/consolidate", body.dump());
    if (resp.status != 200) {
        LOG(WARN) << "[HttpMemoryRuntime] Consolidate HTTP " << resp.status << ": " << resp.body;
        return false;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(resp.body);
        if (!IsSuccessEnvelope(j)) {
            LOG(WARN) << "[HttpMemoryRuntime] Consolidate failed: " << ExtractError(j);
            return false;
        }
        int processed = 0, savedSummaries = 0, savedEntities = 0, savedRelations = 0;
        if (j.contains("processedEvents")) processed = j["processedEvents"].get<int>();
        if (j.contains("savedSummaries")) savedSummaries = j["savedSummaries"].get<int>();
        if (j.contains("savedEntities")) savedEntities = j["savedEntities"].get<int>();
        if (j.contains("savedRelations")) savedRelations = j["savedRelations"].get<int>();
        LOG(INFO) << "[HttpMemoryRuntime] Consolidate ok: processed=" << processed
                  << " summaries=" << savedSummaries << " entities=" << savedEntities
                  << " relations=" << savedRelations;
        return true;
    } catch (const std::exception& e) {
        LOG(WARN) << "[HttpMemoryRuntime] Consolidate parse error: " << e.what();
        return false;
    }
}

std::vector<MemorySearchHit> HttpMemoryRuntime::SearchMemory(const MemorySearchRequest& request)
{
    nlohmann::json body = SerializeSearchRequest(request);
    auto resp = HttpPost("/v1/search", body.dump());
    if (resp.status != 200) {
        LOG(WARN) << "[HttpMemoryRuntime] SearchMemory HTTP " << resp.status << ": " << resp.body;
        return {};
    }
    try {
        nlohmann::json j = nlohmann::json::parse(resp.body);
        if (!IsSuccessEnvelope(j)) {
            LOG(WARN) << "[HttpMemoryRuntime] SearchMemory failed: " << ExtractError(j);
            return {};
        }
        return DeserializeSearchHits(j);
    } catch (const std::exception& e) {
        LOG(WARN) << "[HttpMemoryRuntime] SearchMemory parse error: " << e.what();
        return {};
    }
}

MemoryStats HttpMemoryRuntime::GetStats() const
{
    auto resp = HttpGet("/v1/stats");
    if (resp.status != 200) {
        LOG(WARN) << "[HttpMemoryRuntime] GetStats HTTP " << resp.status << ": " << resp.body;
        MemoryStats local;
        local.appendFailures = circuit_.appendFailures.load();
        local.writeFailures = circuit_.writeFailures.load();
        local.buildContextFailures = circuit_.buildContextFailures.load();
        return local;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(resp.body);
        if (!IsSuccessEnvelope(j)) {
            LOG(WARN) << "[HttpMemoryRuntime] GetStats failed: " << ExtractError(j);
            MemoryStats local;
            local.appendFailures = circuit_.appendFailures.load();
            local.writeFailures = circuit_.writeFailures.load();
            local.buildContextFailures = circuit_.buildContextFailures.load();
            return local;
        }
        MemoryStats stats = DeserializeStats(j);
        stats.appendFailures += circuit_.appendFailures.load();
        stats.writeFailures += circuit_.writeFailures.load();
        stats.buildContextFailures += circuit_.buildContextFailures.load();
        return stats;
    } catch (const std::exception& e) {
        LOG(WARN) << "[HttpMemoryRuntime] GetStats parse error: " << e.what();
        MemoryStats local;
        local.appendFailures = circuit_.appendFailures.load();
        local.writeFailures = circuit_.writeFailures.load();
        local.buildContextFailures = circuit_.buildContextFailures.load();
        return local;
    }
}

} // namespace jiuwen

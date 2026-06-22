#pragma once

#include <string>
#include <vector>

#include "include/memory_runtime.h"

#include <nlohmann/json.hpp>

namespace jiuwen {

class HttpMemoryRuntime : public MemoryRuntime
{
public:
    explicit HttpMemoryRuntime(MemoryConfig config);
    ~HttpMemoryRuntime() override;

    bool AppendEvent(const MemoryEvent& event) override;
    MemoryContextPackage BuildContext(const MemoryContextRequest& request) override;
    MemoryPayloadWriteResult WritePayload(const MemoryPayloadWriteRequest& request) override;
    std::string ReadPayload(const std::string& uri) override;
    bool Consolidate(const MemoryConsolidationRequest& request, MemoryModelClient* modelClient) override;
    std::vector<MemorySearchHit> SearchMemory(const MemorySearchRequest& request) override;
    MemoryStats GetStats() const override;

private:
    std::string serverUrl_;
    std::string apiKey_;
    int timeoutSeconds_;

    struct HttpResponse { long status; std::string body; };
    HttpResponse HttpPost(const std::string& path, const std::string& jsonBody) const;
    HttpResponse HttpGet(const std::string& path) const;

    nlohmann::json SerializeEvent(const MemoryEvent& event) const;
    nlohmann::json SerializeContextRequest(const MemoryContextRequest& request) const;
    nlohmann::json SerializePayloadWriteRequest(const MemoryPayloadWriteRequest& request) const;
    nlohmann::json SerializeConsolidationRequest(const MemoryConsolidationRequest& request) const;
    nlohmann::json SerializeSearchRequest(const MemorySearchRequest& request) const;
    MemoryContextPackage DeserializeContextPackage(const nlohmann::json& j) const;
    MemoryPayloadWriteResult DeserializePayloadWriteResult(const nlohmann::json& j) const;
    std::vector<MemorySearchHit> DeserializeSearchHits(const nlohmann::json& j) const;
    MemoryStats DeserializeStats(const nlohmann::json& j) const;
};

} // namespace jiuwen

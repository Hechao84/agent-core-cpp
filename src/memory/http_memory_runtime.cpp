#include "src/memory/http_memory_runtime.h"

#include <utility>

#include "agent_memory/http_memory_runtime.h"

namespace jiuwen {

namespace {

agent_memory::MemoryConfig ToAgentMemoryConfig(const MemoryConfig& config)
{
    agent_memory::MemoryConfig out;
    out.enabled = config.enabled;
    out.mode = config.mode;
    out.provider = config.provider;
    out.dataPath = config.dataPath;
    out.serverUrl = config.serverUrl;
    out.serverApiKey = config.serverApiKey;
    out.serverTimeoutSeconds = config.serverTimeoutSeconds;
    out.tokenBudget = config.tokenBudget;
    out.hotMessages = config.hotMessages;
    out.compressAfterTokens = config.compressAfterTokens;
    out.offloadToolResultChars = config.offloadToolResultChars;
    out.enablePayloadOffload = config.enablePayloadOffload;
    out.enableShortTermCompression = config.enableShortTermCompression;
    out.enableHierarchicalSummary = config.enableHierarchicalSummary;
    out.enableEntityGraph = config.enableEntityGraph;
    return out;
}

} // namespace

HttpMemoryRuntime::HttpMemoryRuntime(MemoryConfig config)
    : MemoryRuntime(std::move(config)), impl_(std::make_unique<agent_memory::HttpMemoryRuntime>(ToAgentMemoryConfig(config_)))
{
}

HttpMemoryRuntime::~HttpMemoryRuntime() = default;

bool HttpMemoryRuntime::AppendEvent(const MemoryEvent& event)
{
    return impl_->AppendEvent(event);
}

MemoryContextPackage HttpMemoryRuntime::BuildContext(const MemoryContextRequest& request)
{
    return impl_->BuildContext(request);
}

MemoryPayloadWriteResult HttpMemoryRuntime::WritePayload(const MemoryPayloadWriteRequest& request)
{
    return impl_->WritePayload(request);
}

std::string HttpMemoryRuntime::ReadPayload(const std::string& ref)
{
    return impl_->ReadPayload(ref);
}

bool HttpMemoryRuntime::Consolidate(const MemoryConsolidationRequest& request)
{
    return impl_->Consolidate(request);
}

std::vector<MemorySearchResult> HttpMemoryRuntime::SearchMemory(const MemorySearchRequest& request)
{
    return impl_->SearchMemory(request);
}

MemoryStats HttpMemoryRuntime::GetStats() const
{
    return impl_->GetStats();
}

} // namespace jiuwen

#include "src/memory/builtin_memory_runtime.h"

#include <utility>

#include "src/memory/type_bridge.h"
#include "src/utils/logger.h"

#include "agent_memory/builtin_memory_runtime.h"
#include "agent_memory/context.h"
#include "agent_memory/error.h"
#include "agent_memory/long_term_memory.h"
#include "agent_memory/model_client.h"
#include "agent_memory/payload.h"
#include "agent_memory/search.h"
#include "agent_memory/stats.h"

namespace jiuwen {

namespace {

// Adapts a jiuwen::MemoryModelClient to the agent_memory::MemoryModelClient
// interface so a host-supplied model can drive agent-memory-cpp consolidation.
class AgentMemoryModelClientAdapter : public agent_memory::MemoryModelClient
{
public:
    explicit AgentMemoryModelClientAdapter(jiuwen::MemoryModelClient* hostClient)
        : hostClient_(hostClient)
    {}

    agent_memory::ModelInvokeResult GenerateMemoryUpdate(const std::string& prompt) override
    {
        jiuwen::MemoryModelResult r = hostClient_->GenerateMemoryUpdate(prompt);
        agent_memory::ModelInvokeResult out;
        out.text = r.text;
        out.httpStatus = r.httpStatus;
        out.errorCode = r.errorCode;
        out.errorMessage = r.errorMessage;
        out.providerError = r.providerError;
        return out;
    }

private:
    jiuwen::MemoryModelClient* hostClient_;
};

} // namespace

BuiltinMemoryRuntime::BuiltinMemoryRuntime(MemoryConfig config)
    : MemoryRuntime(std::move(config)),
      impl_(std::make_unique<agent_memory::BuiltinMemoryRuntime>(ToAgentMemoryConfig(config_)))
{}

BuiltinMemoryRuntime::~BuiltinMemoryRuntime() = default;

bool BuiltinMemoryRuntime::AppendEvent(const MemoryEvent& event)
{
    auto r = impl_->AppendEvent(ToAgentEvent(event));
    if (!r.succeeded) { LOG(WARN) << "[MemoryRuntime] AppendEvent failed: " << r.error.message; }
    return r.succeeded;
}

MemoryContextPackage BuiltinMemoryRuntime::BuildContext(const MemoryContextRequest& request)
{
    auto r = impl_->BuildContext(ToAgentContextRequest(request));
    if (!r) { LOG(WARN) << "[MemoryRuntime] BuildContext failed: " << r.error.message; return {}; }
    return FromAgentContextPackage(r.context);
}

MemoryPayloadWriteResult BuiltinMemoryRuntime::WritePayload(const MemoryPayloadWriteRequest& request)
{
    return FromAgentPayloadWriteResult(impl_->WritePayload(ToAgentPayloadWriteRequest(request)));
}

std::string BuiltinMemoryRuntime::ReadPayload(const std::string& uri)
{
    auto r = impl_->ReadPayload(uri);
    if (!r) { LOG(WARN) << "[MemoryRuntime] ReadPayload failed: " << r.error.message; return ""; }
    return r.content;
}

bool BuiltinMemoryRuntime::Consolidate(const MemoryConsolidationRequest& request, MemoryModelClient* modelClient)
{
    agent_memory::MemoryConsolidationRequest agentRequest = ToAgentConsolidationRequest(request);
    agent_memory::MemoryConsolidationResult r;
    if (modelClient == nullptr) {
        // nullptr -> runtime decides: use the configured built-in model when
        // available, otherwise rule-based extraction.
        r = impl_->Consolidate(agentRequest);
    } else {
        AgentMemoryModelClientAdapter adapter(modelClient);
        r = impl_->Consolidate(agentRequest, &adapter);
    }
    if (!r) {
        LOG(WARN) << "[MemoryRuntime] Consolidate failed: " << r.error.message;
    } else {
        LOG(INFO) << "[MemoryRuntime] Consolidate ok: processed=" << r.processedEvents
                  << " summaries=" << r.savedSummaries << " entities=" << r.savedEntities
                  << " relations=" << r.savedRelations;
    }
    return r.succeeded;
}

std::vector<MemorySearchHit> BuiltinMemoryRuntime::SearchMemory(const MemorySearchRequest& request)
{
    auto r = impl_->SearchMemory(ToAgentSearchRequest(request));
    if (!r) { LOG(WARN) << "[MemoryRuntime] SearchMemory failed: " << r.error.message; return {}; }
    std::vector<MemorySearchHit> hits;
    hits.reserve(r.hits.size());
    for (const auto& h : r.hits) {
        hits.push_back(FromAgentSearchHit(h));
    }
    return hits;
}

MemoryStats BuiltinMemoryRuntime::GetStats() const
{
    auto r = impl_->GetStats();
    if (!r) { LOG(WARN) << "[MemoryRuntime] GetStats failed: " << r.error.message; return {}; }
    return FromAgentStats(r.stats);
}

} // namespace jiuwen

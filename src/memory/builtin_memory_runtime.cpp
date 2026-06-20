#include "src/memory/builtin_memory_runtime.h"

#include <utility>

#include "agent_memory/builtin_memory_runtime.h"
#include "agent_memory/error.h"
#include "agent_memory/context.h"
#include "agent_memory/search.h"
#include "agent_memory/stats.h"
#include "agent_memory/payload.h"
#include "agent_memory/long_term_memory.h"
#include "include/model.h"
#include "src/utils/logger.h"

namespace jiuwen {

namespace {

agent_memory::MemoryConfig ToAgentMemoryConfig(const jiuwen::MemoryConfig& cfg)
{
    agent_memory::MemoryConfig out;
    out.dataPath = cfg.dataPath;
    out.tokenBudget = cfg.tokenBudget;
    out.offloadThresholdChars = cfg.offloadToolResultChars;
    out.enablePayloadOffload = cfg.enablePayloadOffload;
    out.model.enabled = cfg.modelEnabled;
    out.model.formatType = cfg.modelFormatType;
    out.model.baseUrl = cfg.modelBaseUrl;
    out.model.apiKey = cfg.modelApiKey;
    out.model.modelName = cfg.modelName;
    out.model.organization = cfg.modelOrganization;
    out.model.anthropicVersion = cfg.modelAnthropicVersion;
    out.model.timeoutSeconds = cfg.modelTimeoutSeconds;
    out.model.temperature = cfg.modelTemperature;
    out.model.maxTokens = cfg.modelMaxTokens;
    return out;
}

class JiuwenMemoryModelClient : public agent_memory::MemoryModelClient
{
public:
    explicit JiuwenMemoryModelClient(Model* model) : model_(model) {}

    ModelInvokeResult GenerateMemoryUpdate(const std::string& prompt) override
    {
        ModelInvokeResult result;
        if (!model_) {
            result.errorCode = "null_model";
            result.errorMessage = "No model provided";
            return result;
        }
        std::string systemPrompt = "Follow the instructions in the user message precisely.";
        Message userMsg;
        userMsg.role = "user";
        userMsg.content = prompt;
        std::string formatted = model_->Format(systemPrompt, {userMsg}, {});
        ModelResponse response = model_->Invoke(formatted, nullptr);
        result.text = response.content;
        result.httpStatus = 200;
        if (response.content.empty()) {
            result.errorCode = "empty_response";
            result.errorMessage = "Model returned empty content";
        }
        return result;
    }

private:
    Model* model_;
};

} // namespace

BuiltinMemoryRuntime::BuiltinMemoryRuntime(MemoryConfig config)
    : MemoryRuntime(std::move(config)),
      impl_(std::make_unique<agent_memory::BuiltinMemoryRuntime>(ToAgentMemoryConfig(config_)))
{}

BuiltinMemoryRuntime::~BuiltinMemoryRuntime() = default;

bool BuiltinMemoryRuntime::AppendEvent(const MemoryEvent& event)
{
    auto r = impl_->AppendEvent(event);
    if (!r.succeeded) { LOG(WARN) << "[MemoryRuntime] AppendEvent failed: " << r.error.message; }
    return r.succeeded;
}

MemoryContextPackage BuiltinMemoryRuntime::BuildContext(const MemoryContextRequest& request)
{
    auto r = impl_->BuildContext(request);
    if (!r) { LOG(WARN) << "[MemoryRuntime] BuildContext failed: " << r.error.message; return {}; }
    return r.context;
}

MemoryPayloadWriteResult BuiltinMemoryRuntime::WritePayload(const MemoryPayloadWriteRequest& request)
{
    return impl_->WritePayload(request);
}

std::string BuiltinMemoryRuntime::ReadPayload(const std::string& uri)
{
    auto r = impl_->ReadPayload(uri);
    if (!r) { LOG(WARN) << "[MemoryRuntime] ReadPayload failed: " << r.error.message; return ""; }
    return r.content;
}

bool BuiltinMemoryRuntime::Consolidate(const MemoryConsolidationRequest& request)
{
    auto r = impl_->Consolidate(request);
    if (!r) { LOG(WARN) << "[MemoryRuntime] Consolidate failed: " << r.error.message; }
    else { LOG(INFO) << "[MemoryRuntime] Consolidate ok: processed=" << r.processedEvents
                     << " summaries=" << r.savedSummaries << " entities=" << r.savedEntities
                     << " relations=" << r.savedRelations; }
    return r.succeeded;
}

bool BuiltinMemoryRuntime::Consolidate(const MemoryConsolidationRequest& request, Model* model)
{
    if (!model) { return Consolidate(request); }
    JiuwenMemoryModelClient client(model);
    auto r = impl_->Consolidate(request, &client);
    if (!r) { LOG(WARN) << "[MemoryRuntime] Consolidate with model failed: " << r.error.message; }
    else { LOG(INFO) << "[MemoryRuntime] Consolidate with model ok: processed=" << r.processedEvents
                     << " summaries=" << r.savedSummaries << " entities=" << r.savedEntities
                     << " relations=" << r.savedRelations; }
    return r.succeeded;
}

std::vector<MemorySearchHit> BuiltinMemoryRuntime::SearchMemory(const MemorySearchRequest& request)
{
    auto r = impl_->SearchMemory(request);
    if (!r) { LOG(WARN) << "[MemoryRuntime] SearchMemory failed: " << r.error.message; return {}; }
    return r.hits;
}

MemoryStats BuiltinMemoryRuntime::GetStats() const
{
    auto r = impl_->GetStats();
    if (!r) { LOG(WARN) << "[MemoryRuntime] GetStats failed: " << r.error.message; return {}; }
    return r.stats;
}

} // namespace jiuwen

#include "src/memory/builtin_memory_runtime.h"

#include <utility>

#include "agent_memory/builtin_memory_runtime.h"
#include "include/model.h"

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

class JiuwenMemoryModelClient : public agent_memory::MemoryModelClient
{
public:
    explicit JiuwenMemoryModelClient(Model* model) : model_(model) {}

    std::string InvokeMemoryExtraction(const std::string& prompt) override
    {
        if (!model_) {
            return "";
        }
        Message userMsg;
        userMsg.role = "user";
        userMsg.content = prompt;
        std::string systemPrompt = "You are a memory extraction assistant. Output ONLY valid JSON.";
        std::string formatted = model_->Format(systemPrompt, {userMsg}, {});
        ModelResponse response = model_->Invoke(formatted, nullptr);
        return response.content;
    }

private:
    Model* model_;
};

} // namespace

BuiltinMemoryRuntime::BuiltinMemoryRuntime(MemoryConfig config)
    : MemoryRuntime(std::move(config)), impl_(std::make_unique<agent_memory::BuiltinMemoryRuntime>(ToAgentMemoryConfig(config_)))
{
}

BuiltinMemoryRuntime::~BuiltinMemoryRuntime() = default;

bool BuiltinMemoryRuntime::AppendEvent(const MemoryEvent& event)
{
    return impl_->AppendEvent(event);
}

MemoryContextPackage BuiltinMemoryRuntime::BuildContext(const MemoryContextRequest& request)
{
    return impl_->BuildContext(request);
}

MemoryPayloadWriteResult BuiltinMemoryRuntime::WritePayload(const MemoryPayloadWriteRequest& request)
{
    return impl_->WritePayload(request);
}

std::string BuiltinMemoryRuntime::ReadPayload(const std::string& ref)
{
    return impl_->ReadPayload(ref);
}

bool BuiltinMemoryRuntime::Consolidate(const MemoryConsolidationRequest& request)
{
    return impl_->Consolidate(request);
}

bool BuiltinMemoryRuntime::Consolidate(const MemoryConsolidationRequest& request, Model* model)
{
    JiuwenMemoryModelClient client(model);
    return impl_->Consolidate(request, model ? &client : nullptr);
}

std::vector<MemorySearchResult> BuiltinMemoryRuntime::SearchMemory(const MemorySearchRequest& request)
{
    return impl_->SearchMemory(request);
}

MemoryStats BuiltinMemoryRuntime::GetStats() const
{
    return impl_->GetStats();
}

} // namespace jiuwen

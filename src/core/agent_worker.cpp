#include "src/core/agent_worker.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "include/model.h"
#include "include/resource_manager.h"
#include "src/context_engine/context_engine.h"
#include "src/core/ask_user_dispatcher.h"
#include "src/core/session_todo_list.h"
#include "src/core/tool_turn_state.h"
#include "src/core/worker_env.h"
#include "src/skills/skill_engine.h"
#include "src/tools/tool_selector.h"
#include "src/utils/logger.h"
#include "src/utils/time_utils.h"
#include "src/utils/prompt_utils.h"

namespace fs = std::filesystem;

namespace jiuwen {

AgentWorker::AgentWorker(AgentConfig config) : config_(std::move(config))
{
    toolSelector_ = std::make_unique<ToolSelector>();
    // v1 fallback warnings (one-shot per Agent construction; ReloadAgent
    // re-constructs so a reloaded config re-evaluates). AUTO is not
    // implemented (no budget-driven selection), so it degrades to DISABLED
    // behavior. SELECTIVE is implemented for the FC/catalog mechanics but
    // findRelevant (turn-start active-set seeding) is v2; in v1 SELECTIVE
    // falls back to PROGRESSIVE active-set behavior (seed full pool).
    if (config_.toolDisclosureMode == ToolDisclosureMode::AUTO) {
        LOG(WARN) << "[AgentWorker] toolDisclosureMode=auto is not implemented in v1; "
                  << "falling back to disabled behavior. Set disabled/progressive/selective explicitly.";
    } else if (config_.toolDisclosureMode == ToolDisclosureMode::SELECTIVE) {
        LOG(WARN) << "[AgentWorker] toolDisclosureMode=selective: findRelevant is v2; v1 seeds the "
                  << "active set with the full pool (progressive behavior). FC/catalog mechanics are active.";
    }
}

void AgentWorker::Cancel()
{
    cancelGeneration_.fetch_add(100, std::memory_order_relaxed);
}

void AgentWorker::AddTools(const std::vector<std::string>& toolNames)
{
    auto& rm = ResourceManager::GetInstance();
    // Validate outside the lock: HasTool / HasSessionTool acquire
    // ResourceManager::toolMutex_, so holding AgentWorker::toolMutex_ across
    // them would nest two L5 locks. Mirrors CreateTool / GetToolSchemaForQuery
    // (lock-external RM access per the lock ordering protocol).
    std::vector<std::string> valid;
    for (const auto& name : toolNames) {
        if (rm.HasTool(name) || rm.HasSessionTool(name)) {
            valid.push_back(name);
        } else {
            std::cerr << "Warning: Tool '" << name << "' not found" << std::endl;
        }
    }
    std::lock_guard<std::mutex> lock(toolMutex_);
    for (const auto& name : valid) {
        if (std::find(toolNames_.begin(), toolNames_.end(), name) != toolNames_.end()) {
            continue;
        }
        toolNames_.push_back(name);
        toolSelector_->AddToolToPool(name);
    }
}

void AgentWorker::RemoveTools(const std::vector<std::string>& toolNames)
{
    std::lock_guard<std::mutex> lock(toolMutex_);
    for (const auto& name : toolNames) {
        toolNames_.erase(std::remove(toolNames_.begin(), toolNames_.end(), name), toolNames_.end());
        if (toolSelector_) {
            toolSelector_->RemoveToolFromPool(name);
        }
    }
}

std::vector<std::string> AgentWorker::GetToolNames() const
{
    std::lock_guard<std::mutex> lock(toolMutex_);
    return toolNames_;
}

int AgentWorker::SyncMcpTools()
{
    auto currentSet = ResourceManager::GetInstance().GetMcpToolNames();
    std::unordered_set<std::string> desired(currentSet.begin(), currentSet.end());

    std::vector<std::string> toAdd;
    for (const auto& name : currentSet) {
        if (std::find(ownedMcpTools_.begin(), ownedMcpTools_.end(), name) == ownedMcpTools_.end()) {
            toAdd.push_back(name);
        }
    }
    std::vector<std::string> toRemove;
    for (const auto& name : ownedMcpTools_) {
        if (desired.find(name) == desired.end()) {
            toRemove.push_back(name);
        }
    }

    std::lock_guard<std::mutex> lock(toolMutex_);
    for (const auto& name : toAdd) {
        if (std::find(toolNames_.begin(), toolNames_.end(), name) == toolNames_.end()) {
            toolNames_.push_back(name);
            if (toolSelector_) toolSelector_->AddToolToPool(name);
        }
        ownedMcpTools_.push_back(name);
    }
    for (const auto& name : toRemove) {
        toolNames_.erase(std::remove(toolNames_.begin(), toolNames_.end(), name), toolNames_.end());
        if (toolSelector_) toolSelector_->RemoveToolFromPool(name);
        ownedMcpTools_.erase(std::remove(ownedMcpTools_.begin(), ownedMcpTools_.end(), name), ownedMcpTools_.end());
    }
    return static_cast<int>(toAdd.size() + toRemove.size());
}

void AgentWorker::SetSkillEngine(std::shared_ptr<SkillEngine> engine)
{
    skillEngine_ = engine;
}

void AgentWorker::SetWorkerEnv(WorkerEnv* env)
{
    workerEnv_ = env;
}

bool AgentWorker::IsProgressiveDisclosureActive() const
{
    // AUTO resolves to DISABLED in v1 (no budget-driven selection); the
    // constructor logs the fallback. PROGRESSIVE and SELECTIVE both use the
    // Tier 1 catalog + load + tool_search mechanics (SELECTIVE's findRelevant
    // is the v2 delta, orthogonal to this FC/catalog branch).
    return config_.toolDisclosureMode == ToolDisclosureMode::PROGRESSIVE
        || config_.toolDisclosureMode == ToolDisclosureMode::SELECTIVE;
}

std::set<std::string> AgentWorker::ComputeAlwaysOn() const
{
    return ComputeAlwaysOnFor(config_);
}

std::set<std::string> ComputeAlwaysOnFor(const AgentConfig& config)
{
    std::set<std::string> alwaysOn = MetaToolNames();
    for (const auto& t : config.alwaysOnTools) {
        alwaysOn.insert(t);
    }
    return alwaysOn;
}

const std::set<std::string>& MetaToolNames()
{
    static const std::set<std::string> kMetaTools = {"tool_search", "skill_search"};
    return kMetaTools;
}

std::vector<ToolSchema> AgentWorker::BuildToolSchemas() const
{
    if (!IsProgressiveDisclosureActive()) {
        // disabled / auto(v1→disabled): full toolNames_ resident (current
        // behavior, zero regression). Snapshot under toolMutex_, call RM
        // outside the lock (RM acquires its own toolMutex_).
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> lock(toolMutex_);
            names = toolNames_;
        }
        return ResourceManager::GetInstance().BuildToolSchemas(names);
    }
    // progressive/selective: FC = alwaysOn ∪ loadedTools. loadedTools lives
    // on the current SessionEntry (per-turn, read via TLS); alwaysOn is
    // computed here from config_. Both contribute only names; RM renders
    // the full schemas (and skips unknown names silently, so an alwaysOn
    // name that isn't registered — e.g. tool_search under a misconfig — is
    // just absent rather than crashing).
    std::set<std::string> fcNames = ComputeAlwaysOn();
    if (workerEnv_ != nullptr) {
        ToolTurnState* ts = workerEnv_->GetCurrentTurnState();
        if (ts != nullptr) {
            const auto& loaded = ts->getLoadedTools();
            fcNames.insert(loaded.begin(), loaded.end());
        }
    }
    std::vector<std::string> names(fcNames.begin(), fcNames.end());
    return ResourceManager::GetInstance().BuildToolSchemas(names);
}

ModelResponse AgentWorker::CallModelStream(const std::string& systemPrompt,
                                            const std::vector<Message>& messages,
                                            std::function<void(const std::string&)> onChunk,
                                            uint64_t generation)
{
    ModelResponse out;
    if (IsCancelled(generation)) {
        out.finishReason = "cancelled";
        out.isFinished = true;
        return out;
    }
    try {
        auto model = ResourceManager::GetInstance().CreateModel(config_.modelConfig);
        auto tools = BuildToolSchemas();
        std::string formatted = model->Format(systemPrompt, messages, tools);
        LOG(INFO) << "Request Model Prompt:\n" << formatted;
        // Propagate the session's cancel state into the streaming HTTP transfer
        // so a Cancel() aborts the in-flight model call mid-stream, not just
        // between iterations. The front-gate IsCancelled above already covers
        // the "cancelled before any work" case; this covers "cancelled while
        // the model is still streaming".
        auto shouldCancel = [this, generation]() -> bool {
            return IsCancelled(generation);
        };
        out = model->Invoke(formatted, onChunk, shouldCancel);
        LOG(INFO) << "Model returned " << out.content.length() << " content chars; "
                  << out.toolCalls.size() << " tool_calls; finish_reason=" << out.finishReason
                  << "; content=[" << out.content << "]";
    } catch (const std::exception& e) {
        out.content = std::string("Model Error: ") + e.what();
        out.finishReason = "error";
        out.isFinished = true;
        if (onChunk) {
            onChunk(out.content);
        }
    }
    return out;
}

std::string AgentWorker::BuildPrompt(const std::string& templateName, const std::string& query,
                                    const std::string& context, ContextEngine* contextEngine)
{
    // 1. Resolve the template content (load from file if configured, or fallback to templates/REACT_SYSTEM.md)
    std::string promptTemplate;
    auto it = config_.promptTemplates.find(templateName);
    if (it != config_.promptTemplates.end()) {
        promptTemplate = ResolvePromptResource(it->second);
    } else {
        // Fallback: load from templates/REACT_SYSTEM.md relative to current working directory
        fs::path fallbackPath = fs::current_path() / "templates" / "REACT_SYSTEM.md";
        if (fs::exists(fallbackPath)) {
            std::ifstream file(fallbackPath);
            if (file.is_open()) {
                std::stringstream buffer;
                buffer << file.rdbuf();
                promptTemplate = buffer.str();
            }
        }
    }

    if (promptTemplate.empty()) {
        return query;
    }

    // 2. Prepare variables for rendering
    std::unordered_map<std::string, std::string> vars;
    vars["query"] = query;
    vars["context"] = context;

    // Runtime context variables
    vars["current_time"] = jiuwen::NowUtcIso8601();
    vars["session_id"] = config_.contextConfig.sessionId;

    // 3. Resolve sub-templates from config (e.g., {$identity}, {$custom_section})
    for (const auto& tpl : config_.promptTemplates) {
        if (tpl.first == templateName) {
            continue;
        }
        vars[tpl.first] = ResolvePromptResource(tpl.second);
    }

    // 4. Hot-reload skills and load into {$skills}
    if (skillEngine_) {
        skillEngine_->Load(true);
        vars["skills"] = skillEngine_->GetSkillCatalog();
    } else {
        vars["skills"] = "";
    }

    // 6. Get tool schema into {$tools}
    if (IsProgressiveDisclosureActive() && workerEnv_ != nullptr) {
        // progressive/selective: Tier 1 catalog = name+desc (no params) for
        // visibleNames = active ∪ alwaysOn. BuildPrompt runs every React
        // iteration (react_worker.cpp:89, inside the for loop), so the
        // catalog reflects the current active set (which grows via
        // tool_search.search across iterations). Native FC: flat list
        // (protocol restricts calls). Prompt-mode: split into "directly
        // callable" / "requires load first" sections.
        ToolTurnState* ts = workerEnv_->GetCurrentTurnState();
        if (ts != nullptr) {
            const auto& active = ts->getActiveSet();
            const auto& loaded = ts->getLoadedTools();
            auto alwaysOn = ComputeAlwaysOn();
            std::set<std::string> visible, callable;
            visible.insert(active.begin(), active.end());
            visible.insert(alwaysOn.begin(), alwaysOn.end());
            callable.insert(loaded.begin(), loaded.end());
            callable.insert(alwaysOn.begin(), alwaysOn.end());
            auto renderMode = config_.modelConfig.useNativeFunctionCalling
                ? ResourceManager::CatalogRenderMode::NativeFc
                : ResourceManager::CatalogRenderMode::PromptMode;
            vars["tools"] = ResourceManager::GetInstance().GetToolCatalog(visible, renderMode, callable);
        } else {
            // Outside an Invoke context (offline probe): fall back to full
            // schemas so BuildPrompt still produces something usable.
            vars["tools"] = GetToolSchemaForQuery(query);
        }
    } else {
        // disabled / auto(v1→disabled): full schemas resident (current
        // behavior, zero regression).
        vars["tools"] = GetToolSchemaForQuery(query);
    }

    // 7. Load memory context into {$memory}
    if (contextEngine) {
        std::string memoryContent = contextEngine->GetMemoryContent();
        if (!memoryContent.empty()) {
            vars["memory"] = "# Long-term Memory\n\n" + memoryContent;
        } else {
            vars["memory"] = "";
        }
    } else {
        vars["memory"] = "";
    }

    // 8. Render the prompt with all variables
    std::string rendered = RenderPrompt(promptTemplate, vars);

    // 9. Append the per-session todo snippet (if any) so the model is aware
    // of its plan without requiring the template author to add a placeholder.
    std::string todoSnippet = GetTodoSnippet();
    if (!todoSnippet.empty()) {
        rendered += "\n\n" + todoSnippet;
    }
    return rendered;
}

std::string AgentWorker::GetTodoSnippet() const
{
    if (workerEnv_ == nullptr) {
        return "";
    }
    // Only emit when this worker has a todo_* tool registered, so todos do
    // not leak into agents that did not opt in.
    {
        std::lock_guard<std::mutex> lock(toolMutex_);
        bool hasTodoTool = false;
        for (const auto& n : toolNames_) {
            if (n.rfind("todo_", 0) == 0) {
                hasTodoTool = true;
                break;
            }
        }
        if (!hasTodoTool) {
            return "";
        }
    }
    auto* list = workerEnv_->GetOrCreateSessionTodoList(config_.contextConfig.sessionId);
    if (list == nullptr || list->Empty()) {
        return "";
    }
    return list->Render();
}

std::string AgentWorker::ExecuteTool(const std::string& toolName, const std::string& input,
                                     const std::function<void(const std::string&)>& streamCallback)
{
    LOG(INFO) << "[Tool Execute] Tool: " << toolName << ", Input: " << input;

    try {
        auto& rm = ResourceManager::GetInstance();
        std::unique_ptr<Tool> tool;
        if (rm.HasSessionTool(toolName)) {
            ToolBuildContext ctx;
            ctx.sessionId = config_.contextConfig.sessionId;
            ctx.streamCallback = streamCallback;
            if (workerEnv_ != nullptr) {
                ctx.todoList = workerEnv_->GetOrCreateSessionTodoList(ctx.sessionId);
                ctx.askUser = workerEnv_->GetAskUserDispatcher(ctx.sessionId);
                ctx.memoryRuntime = workerEnv_->GetMemoryRuntime();
                ctx.skillEngine = workerEnv_->GetSkillEngine();
                // Per-turn disclosure state (§5.2 turnState channel). The
                // proxy on the current SessionEntry (lazily created by
                // SmWorkerEnv::GetCurrentTurnState); tool_search reads
                // getActiveSet/getLoadedTools/isActiveFullPool through it.
                // The short-circuit decision for search is based on
                // isActiveFullPool() (runtime state), not a mode label, so
                // no toolDisclosureMode value is threaded into the ctx.
                ctx.turnState = workerEnv_->GetCurrentTurnState();
            }
            tool = rm.CreateSessionTool(toolName, ctx);
        } else {
            tool = rm.CreateTool(toolName);
        }
        std::string result = tool->Invoke(input);

        LOG(INFO) << "[Tool Result] Tool: " << toolName << ", Output length: " << result.length();
        LOG(INFO) << "[Tool Result Full] Tool: " << toolName << "\n" << result;

        return result;
    } catch (const std::exception& e) {
        std::string errMsg = "Error executing tool '" + toolName + "': " + e.what();
        LOG(ERR) << "[Tool Error] " << errMsg;
        return errMsg;
    }
}

std::string AgentWorker::GetToolSchemaForQuery(const std::string& query)
{
    (void)query;
    // Snapshot toolNames_ under toolMutex_, then call ResourceManager outside
    // the lock. HasTool / HasSessionTool / GetToolSchema acquire
    // ResourceManager::toolMutex_, so holding AgentWorker::toolMutex_ across
    // them would nest two L5 locks; the lock-internal-read -> lock-external-
    // call pattern keeps them non-nesting (mirrors BuildToolSchemas /
    // CreateTool).
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(toolMutex_);
        names = toolNames_;
    }
    auto& rm = ResourceManager::GetInstance();
    std::string schema;
    for (const auto& name : names) {
        try {
            if (rm.HasTool(name) || rm.HasSessionTool(name)) {
                schema += rm.GetToolSchema(name) + "\n\n";
            }
        } catch (const std::exception& e) {
            LOG(ERR) << "Failed to get schema for tool: " << name;
        }
    }
    return schema;
}

uint64_t AgentWorker::CurrentCancelGeneration()
{
    // Return the current generation value without incrementing.
    // Incrementing a global counter for every new request would invalidate
    // all other concurrently running sessions sharing this worker.
    // Cancellation is still supported because Cancel() jumps the counter by 100,
    // making cancelGeneration_ exceed every prior myGeneration snapshot.
    return cancelGeneration_.load(std::memory_order_relaxed);
}

bool AgentWorker::IsCancelled(uint64_t myGeneration) const
{
    // True once Cancel() has advanced cancelGeneration_ past this invocation's
    // baseline snapshot. Name and return value agree: cancelled => true.
    return cancelGeneration_.load(std::memory_order_relaxed) > myGeneration;
}

} // namespace jiuwen

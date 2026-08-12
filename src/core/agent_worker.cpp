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
#include "include/config/agent_config_json.h"
#include "include/resource_manager.h"
#include "include/session_manager.h"
#include "src/context_engine/context_engine.h"
#include "src/core/ask_user_dispatcher.h"
#include "src/core/capability_selector.h"
#include "src/core/session_todo_list.h"
#include "src/core/turn_state.h"
#include "src/core/worker_env.h"
#include "src/skills/skill_engine.h"
#include "src/utils/logger.h"
#include "src/utils/time_utils.h"
#include "src/utils/prompt_utils.h"

namespace fs = std::filesystem;

namespace jiuwen {

AgentWorker::AgentWorker(AgentConfig config) : config_(std::move(config))
{
    // V3 (round5 §5.4.2): AUTO is now a real mode — resolves lazily via
    // call_once on first IsProgressiveDisclosureActive() call (effectiveMode_
    // starts as AUTO, gets resolved to DISABLED/PROGRESSIVE/SELECTIVE based
    // on pool token budget). No construction-time WARN; the old "AUTO maps
    // to disabled + warning" v1/v2 fallback is gone.
    // PROGRESSIVE/SELECTIVE skip resolution (effectiveMode_ already equals
    // config_.toolDisclosureMode for those).
    // V2 (round5 §5.4.1 条 10): CapabilitySelector replaces the deprecated
    // ToolSelector. Constructed here with the agent's config (for the
    // modelConfig) and the SkillEngine (set later via SetSkillEngine, which
    // also rebinds the CapabilitySelector's SkillEngine pointer). Lives for
    // the Agent's lifetime; ReloadAgent re-constructs.
    capabilitySelector_ = std::make_unique<CapabilitySelector>(config_, nullptr);
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
    }
}

void AgentWorker::RemoveTools(const std::vector<std::string>& toolNames)
{
    std::lock_guard<std::mutex> lock(toolMutex_);
    for (const auto& name : toolNames) {
        toolNames_.erase(std::remove(toolNames_.begin(), toolNames_.end(), name), toolNames_.end());
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
        }
        ownedMcpTools_.push_back(name);
    }
    for (const auto& name : toRemove) {
        toolNames_.erase(std::remove(toolNames_.begin(), toolNames_.end(), name), toolNames_.end());
        ownedMcpTools_.erase(std::remove(ownedMcpTools_.begin(), ownedMcpTools_.end(), name), ownedMcpTools_.end());
    }
    return static_cast<int>(toAdd.size() + toRemove.size());
}

void AgentWorker::SetSkillEngine(std::shared_ptr<SkillEngine> engine)
{
    skillEngine_ = engine;
    // Rebind the CapabilitySelector's SkillEngine pointer so it tracks the
    // active Agent's SkillEngine (which is Agent-scoped and shared across
    // sessions). ReloadAgent re-constructs CapabilitySelector with nullptr
    // and then this method rebinds it to the new SkillEngine.
    if (capabilitySelector_ != nullptr) {
        capabilitySelector_ = std::make_unique<CapabilitySelector>(config_, engine.get());
    }
}

void AgentWorker::SetWorkerEnv(WorkerEnv* env)
{
    workerEnv_ = env;
}

bool AgentWorker::IsProgressiveDisclosureActive() const
{
    // V3 (round5 §5.4.2): AUTO triggers lazy resolution via std::call_once
    // on first call. effectiveMode_ starts as config_.toolDisclosureMode
    // (AUTO stays unresolved); once call_once runs, effectiveMode_ is
    // resolved to one of DISABLED/PROGRESSIVE/SELECTIVE based on pool token
    // budget via ResolveByBudget. PROG/SEL skip resolution (their
    // effectiveMode_ already equals config_.toolDisclosureMode at
    // construction). Subsequent calls return effectiveMode_ directly
    // (call_once is a no-op for already-resolved once_flag).
    //
    // call_once guarantees one-shot resolution under multi-session concurrent
    // first-call (§5.2: up to 3 concurrent sessions). The lambda body
    // (including the LOG(INFO) at the end) runs exactly once across all
    // concurrent callers — others block on the call_once until the first
    // finishes, then proceed to the return statement with effectiveMode_
    // already populated. This is why the "resolved to <mode>" LOG lives
    // INSIDE the lambda (call_once ensures one emission); putting it
    // outside would re-emit on every IsProgressiveDisclosureActive() call
    // (BuildToolSchemas/BuildPrompt/ExecuteTool ctx fill all call this —
    // would spam logs).
    if (effectiveMode_ == ToolDisclosureMode::AUTO) {
        std::call_once(resolveOnce_, [&] {
            effectiveMode_ = ResolveByBudget(GetToolNames());
            LOG(INFO) << "[AgentWorker] auto resolved to "
                      << ToolDisclosureModeToString(effectiveMode_);
        });
    }
    return effectiveMode_ == ToolDisclosureMode::PROGRESSIVE
        || effectiveMode_ == ToolDisclosureMode::SELECTIVE;
}

ToolDisclosureMode AgentWorker::GetEffectiveMode() const
{
    // Triggers lazy resolution if AUTO; the bool return value of
    // IsProgressiveDisclosureActive() is ignored — only its side effect
    // (populating effectiveMode_ via call_once) matters. Reuses the entry
    // method instead of duplicating call_once logic, so all "read effective
    // mode" sites route through one resolution path.
    IsProgressiveDisclosureActive();
    return effectiveMode_;
}

ToolDisclosureMode AgentWorker::ResolveByBudget(const std::vector<std::string>& pool) const
{
    // V3 (round5 §5.4.2): data-fetch phase — calls RM (non-pure), stays as
    // member. Tier 2 excludes alwaysOn (meta-tools + config_.alwaysOnTools
    // are always FC-resident regardless of mode, so counting them would
    // inflate Tier 2 and may falsely elevate a disabled-sized pool to
    // progressive). Tier 1 (name+desc catalog) does NOT exclude alwaysOn —
    // the catalog naturally contains all tools' name+desc (alwaysOn's
    // name+desc included), progressive Tier 1 is full-pool-visible by design.
    auto alwaysOn = ComputeAlwaysOn();  // MetaToolNames() ∪ config_.alwaysOnTools
    std::vector<std::string> nonAlwaysOn;
    for (const auto& name : pool) {
        if (alwaysOn.find(name) == alwaysOn.end()) {
            nonAlwaysOn.push_back(name);
        }
    }

    auto& rm = ResourceManager::GetInstance();
    // Tier 2: full schema (with params) of non-alwaysOn tools, composed into
    // text and fed to ContextEngine::EstimateTokens.
    auto schemas = rm.BuildToolSchemas(nonAlwaysOn);
    std::string schemaText;
    for (const auto& s : schemas) {
        schemaText += s.name + s.description + s.parameters.dump();
    }
    int tier2 = ContextEngine::EstimateTokens(schemaText);

    // Tier 1: name+desc catalog of ALL tools (alwaysOn included). Visible
    // set = full pool; callable set empty (recall doesn't care about
    // loadedTools); NativeFc render mode for consistency with progressive
    // catalog rendering.
    std::set<std::string> allVisible(pool.begin(), pool.end());
    std::set<std::string> emptyCallable;
    std::string catalogText = rm.GetToolCatalog(allVisible,
        ResourceManager::CatalogRenderMode::NativeFc, emptyCallable);
    int tier1 = ContextEngine::EstimateTokens(catalogText);

    LOG(INFO) << "[AgentWorker] auto tier2=" << tier2 << " tokens (budget "
              << config_.toolSchemaTokenBudget << "), tier1=" << tier1
              << " tokens (budget " << config_.toolCatalogTokenBudget
              << "), pool size=" << pool.size()
              << ", nonAlwaysOn=" << nonAlwaysOn.size();

    return ResolveModeByTokenBudget(tier2, tier1,
                                     config_.toolSchemaTokenBudget,
                                     config_.toolCatalogTokenBudget);
}

// V3 (round5 §5.4.2 条 12): judge phase — pure function, no RM side effects.
// Free function so unit tests can call directly (declared in agent_worker.h,
// same pattern as MetaToolNames). Eats 4 ints (tier2/tier1 token counts +
// schemaBudget/catalogBudget thresholds), returns the resolved mode.
// budget=0 means "this tier's budget unset, skip judgment at that level"
// — both budgets 0 → DISABLED (default AUTO config with no explicit budgets
// = zero +1 LLM/turn, conservative).
ToolDisclosureMode ResolveModeByTokenBudget(int tier2, int tier1,
                                            int schemaBudget, int catalogBudget)
{
    if (schemaBudget > 0 && tier2 > schemaBudget) {
        // Full schemas exceed Tier 2 budget → disabled can't hold, at least
        // progressive (FC = alwaysOn ∪ loadedTools, schema count drops).
        if (catalogBudget > 0 && tier1 > catalogBudget) {
            return ToolDisclosureMode::SELECTIVE;  // Tier 1 also exceeds → progressive's full catalog can't hold → selective
        }
        return ToolDisclosureMode::PROGRESSIVE;  // Tier 2 exceeds but Tier 1 doesn't → progressive (FC drops, Tier 1 catalog still full-pool visible)
    }
    return ToolDisclosureMode::DISABLED;  // Neither exceeds → disabled (full schemas resident, zero +1 LLM/turn)
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
        TurnState* ts = workerEnv_->GetCurrentTurnState();
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
        // V2 (round5 §5.4.1 条 11): by-subset rendering under progressive
        // disclosure. The caller (BuildPrompt) holds workerEnv_, which gives
        // access to the per-turn TurnState via TLS — read the skill active
        // set and pass it as the visible names to the by-subset overload.
        // SkillEngine stays Agent-scoped (no WorkerEnv dependency); the
        // active-set read happens here in the caller.
        //
        // Note: skill side has NO alwaysOn concept — visible = skillActive
        // only (no union with ComputeAlwaysOn()). Rationale: tools need
        // alwaysOn because the FC protocol hard-restricts calls to the FC
        // array (escape valves like tool_search/skill_search must always be
        // callable under SELECTIVE). Skills have no protocol gate — they're
        // text references in the prompt, not callable endpoints. The escape
        // valve for skills IS skill_search (a tool, alwaysOn meta-tool,
        // belongs to the tool-side alwaysOn set); the model uses it to
        // discover/load skills. So a "skill alwaysOn" would be a concept
        // without a structural purpose. Extends the "tool/skill load
        // asymmetry" of §5.4.1 条 11 to the alwaysOn dimension: tools need
        // it (FC gate bypass), skills don't (no gate).
        if (IsProgressiveDisclosureActive() && workerEnv_ != nullptr) {
            TurnState* ts = workerEnv_->GetCurrentTurnState();
            if (ts != nullptr) {
                const auto& skillActive = ts->getSkillActiveSet();
                std::set<std::string> visible(skillActive.begin(), skillActive.end());
                vars["skills"] = skillEngine_->GetSkillCatalog(visible);
            } else {
                // Off-Invoke probe path: fall back to full catalog.
                vars["skills"] = skillEngine_->GetSkillCatalog();
            }
        } else {
            // disabled/auto(v1→disabled): full catalog (V1 behavior).
            vars["skills"] = skillEngine_->GetSkillCatalog();
        }
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
        TurnState* ts = workerEnv_->GetCurrentTurnState();
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
        std::string memoryContent = contextEngine->GetMemoryContent(query);
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
                // V2: capabilitySelector is filled for both tool_search and
                // skill_search's real-recall branch (round5 §5.4.1 条 8/9).
                // Only populated under progressive disclosure (disabled
                // mode keeps stateless substring matching in skill_search).
                ctx.turnState = workerEnv_->GetCurrentTurnState();
                ctx.capabilitySelector = IsProgressiveDisclosureActive()
                    ? capabilitySelector_.get() : nullptr;
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

#include "src/core/agent_worker.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "include/model.h"
#include "include/resource_manager.h"
#include "src/context_engine/context_engine.h"
#include "src/skills/skill_engine.h"
#include "src/tools/tool_selector.h"
#include "src/utils/logger.h"
#include "src/utils/prompt_utils.h"

namespace fs = std::filesystem;

namespace jiuwen {

AgentWorker::AgentWorker(AgentConfig config) : config_(std::move(config))
{
    toolSelector_ = std::make_unique<ToolSelector>();
}

void AgentWorker::Cancel() 
{ 
    cancelled_.store(true); 
}

void AgentWorker::AddTools(const std::vector<std::string>& toolNames)
{
    auto& rm = ResourceManager::GetInstance();
    for (const auto& name : toolNames) {
        if (rm.HasTool(name)) {
            toolNames_.push_back(name);
            toolSelector_->AddToolToPool(name);
        } else {
            std::cerr << "Warning: Tool '" << name << "' not found" << std::endl;
        }
    }
}

void AgentWorker::SetContextEngine(std::shared_ptr<ContextEngine> engine)
{
    contextEngine_ = engine;
}

void AgentWorker::SetSkillEngine(std::shared_ptr<SkillEngine> engine)
{
    skillEngine_ = engine;
}

void AgentWorker::CallModelStream(const std::string& prompt, const std::vector<std::pair<std::string, std::string>>& messages,
                                  std::function<void(const std::string&)> onChunk, std::function<void(const std::string&)> onComplete) 
{
    if (cancelled_.load()) { 
        onComplete(""); 
        return; 
    }
    try {
        auto model = ResourceManager::GetInstance().CreateModel(config_.modelConfig);
        std::vector<Message> msgHistory;
        for (const auto& m : messages) msgHistory.push_back({m.first, m.second});
        std::string formatted = model->Format(prompt, msgHistory);
        
        LOG(INFO) << "Request Model Prompt:\n" << formatted;
        
        std::string fullResponse = model->Invoke(formatted, onChunk);
        
        LOG(INFO) << "Model Response:\n" << fullResponse;
        
        if (onComplete) onComplete(fullResponse);
    } catch (const std::exception& e) {
        std::string err = "Model Error: " + std::string(e.what());
        if (onChunk) onChunk(err);
        if (onComplete) onComplete(err);
    }
}

std::string AgentWorker::BuildPrompt(const std::string& templateName, const std::string& query, const std::string& context)
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

    if (promptTemplate.empty()) return query;

    // 2. Prepare variables for rendering
    std::unordered_map<std::string, std::string> vars;
    vars["query"] = query;
    vars["context"] = context;

    // Runtime context variables
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream timeStream;
    timeStream << std::put_time(std::gmtime(&time), "%Y-%m-%d %H:%M:%S UTC");
    vars["current_time"] = timeStream.str();
    vars["session_id"] = config_.contextConfig.sessionId;

    // 3. Resolve sub-templates from config (e.g., {$identity}, {$custom_section})
    for (const auto& tpl : config_.promptTemplates) {
        if (tpl.first == templateName) continue;
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
    vars["tools"] = GetToolSchemaForQuery(query);

    // 7. Load memory context into {$memory}
    if (contextEngine_) {
        std::string memoryContent = contextEngine_->GetMemoryContent();
        if (!memoryContent.empty()) {
            vars["memory"] = "# Long-term Memory\n\n" + memoryContent;
        } else {
            vars["memory"] = "";
        }
    } else {
        vars["memory"] = "";
    }

    // 8. Render the prompt with all variables
    return RenderPrompt(promptTemplate, vars);
}

std::string AgentWorker::ExecuteTool(const std::string& toolName, const std::string& input)
{
    LOG(INFO) << "[Tool Execute] Tool: " << toolName << ", Input: " << input;

    try {
        auto tool = ResourceManager::GetInstance().CreateTool(toolName);
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
    // TODO: Bypass tool selection for now; dump all registered tools into prompt.
    (void)query;
    auto& rm = ResourceManager::GetInstance();
    std::string schema;
    for (const auto& name : toolNames_) {
        try {
            schema += rm.CreateTool(name)->GetSchema() + "\n\n";
        } catch (const std::exception& e) {
            LOG(ERR) << "Failed to get schema for tool: " << name;
        }
    }
    return schema;
}

} // namespace jiuwen

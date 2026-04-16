
#include "src/core/agent_worker.h"
#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "src/utils/logger.h"
#include "include/model.h"
#include "include/resource_manager.h"
#include "src/context_engine/context_engine.h"
#include "src/skills/skill_engine.h"
#include "src/tools/tool_selector.h"

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
    auto it = config_.promptTemplates.find(templateName);
    if (it == config_.promptTemplates.end()) return query;
    std::string prompt = it->second;
    auto replaceAll = [&](const std::string& from, const std::string& to)
    {
        size_t pos = 0;
        while ((pos = prompt.find(from, pos)) != std::string::npos) {
            prompt.replace(pos, from.length(), to); pos += to.length();
        }
    };

    // 1. Hot-reload skills to pick up file system changes
    if (skillEngine_) {
        skillEngine_->Load(true);
        replaceAll("{skills}", skillEngine_->GetFormattedInstructions());
    } else {
        replaceAll("{skills}", "");
    }

    // 2. Replace standard placeholders
    replaceAll("{query}", query);
    replaceAll("{context}", context);
    replaceAll("{tools}", GetToolSchemaForQuery(query));
    return prompt;
}

std::string AgentWorker::ExecuteTool(const std::string& toolName, const std::string& input)
{
    try {
        auto tool = ResourceManager::GetInstance().CreateTool(toolName);
        return tool->Invoke(input);
    } catch (const std::exception& e) {
        return "Error executing tool '" + toolName + "': " + e.what();
    }
}

std::string AgentWorker::GetToolSchemaForQuery(const std::string& query)
{
    auto& rm = ResourceManager::GetInstance();
    auto selected = toolSelector_->SelectTopTools(query, toolNames_, 3);
    if (selected.empty() && !toolNames_.empty()) selected.push_back(toolNames_[0]);
    std::string schema;
    for (const auto& name : selected) {
        try { schema += rm.CreateTool(name)->GetSchema() + "\n\n"; } catch (...){} }
    return schema;
}

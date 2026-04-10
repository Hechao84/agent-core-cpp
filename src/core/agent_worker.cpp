#include "agent_worker.h"
#include "resource_manager.h"
#include "model.h"
#include "tools/tool_selector.h"
#include "context_engine/context_engine.h"
#include "skills/skill_engine.h"
#include <iostream>
#include <algorithm>
#include <sstream>

AgentWorker::AgentWorker(AgentConfig config) : m_config(std::move(config)) {
    m_toolSelector = std::make_unique<ToolSelector>();
}

void AgentWorker::Cancel() { m_cancelled.store(true); }

void AgentWorker::AddTools(const std::vector<std::string>& toolNames) {
    auto& rm = ResourceManager::GetInstance();
    for (const auto& name : toolNames) {
        if (rm.HasTool(name)) {
            m_toolNames.push_back(name);
            m_toolSelector->AddToolToPool(name);
        } else {
            std::cerr << "Warning: Tool '" << name << "' not found" << std::endl;
        }
    }
}

void AgentWorker::SetContextEngine(std::shared_ptr<ContextEngine> engine) {
    m_contextEngine = engine;
}

void AgentWorker::SetSkillEngine(std::shared_ptr<SkillEngine> engine) {
    m_skillEngine = engine;
}

void AgentWorker::CallModelStream(const std::string& prompt, const std::vector<std::pair<std::string, std::string>>& messages,
                                  std::function<void(const std::string&)> onChunk, std::function<void(const std::string&)> onComplete) {
    if (m_cancelled.load()) { onComplete(""); return; }
    try {
        auto model = ResourceManager::GetInstance().CreateModel(m_config.modelConfig);
        std::vector<Message> msgHistory;
        for (const auto& m : messages) msgHistory.push_back({m.first, m.second});
        std::string formatted = model->Format(prompt, msgHistory);
        std::string fullResponse = model->Invoke(formatted, onChunk);
        if (onComplete) onComplete(fullResponse);
    } catch (const std::exception& e) {
        std::string err = "Model Error: " + std::string(e.what());
        if (onChunk) onChunk(err);
        if (onComplete) onComplete(err);
    }
}

std::string AgentWorker::BuildPrompt(const std::string& templateName, const std::string& query, const std::string& context) {
    auto it = m_config.promptTemplates.find(templateName);
    if (it == m_config.promptTemplates.end()) return query;
    std::string prompt = it->second;
    auto replaceAll = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = prompt.find(from, pos)) != std::string::npos) { prompt.replace(pos, from.length(), to); pos += to.length(); }
    };
    
    // 1. Hot-reload skills to pick up file system changes
    if (m_skillEngine) {
        m_skillEngine->Load(true); 
        replaceAll("{skills}", m_skillEngine->GetFormattedInstructions());
    } else {
        replaceAll("{skills}", "");
    }

    // 2. Replace standard placeholders
    replaceAll("{query}", query); 
    replaceAll("{context}", context);
    replaceAll("{tools}", GetToolSchemaForQuery(query));
    return prompt;
}

std::string AgentWorker::ExecuteTool(const std::string& toolName, const std::string& input) {
    try { auto tool = ResourceManager::GetInstance().CreateTool(toolName); return tool->Invoke(input); }
    catch (const std::exception& e) { return "Error executing tool '" + toolName + "': " + e.what(); }
}

std::string AgentWorker::GetToolSchemaForQuery(const std::string& query) {
    auto& rm = ResourceManager::GetInstance();
    auto selected = m_toolSelector->SelectTopTools(query, m_toolNames, 3);
    if (selected.empty() && !m_toolNames.empty()) selected.push_back(m_toolNames[0]);
    std::string schema;
    for (const auto& name : selected) {
        try { schema += rm.CreateTool(name)->GetSchema() + "\n\n"; } catch (...) {}
    }
    return schema;
}

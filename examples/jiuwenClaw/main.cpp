#include <iostream>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
    #include <windows.h>
#endif

#include "include/agent.h"
#include "include/resource_manager.h"
// Heartbeat management & Cron watcher module
#include "examples/jiuwenClaw/cron_watcher.h"
#include "examples/jiuwenClaw/heartbeat_manager.h"
// Demo-specific tools
#include "examples/jiuwenClaw/tools/cron_tool.h"
#include "examples/jiuwenClaw/tools/notebook_edit_tool.h"
#include "examples/jiuwenClaw/tools/notify_tool.h"

#include "examples/jiuwenClaw/utils/encoding.h"
#include "third_party/include/nlohmann/json.hpp"

using namespace jiuwen;

int main()
{
    std::cout << "jiuwenClaw is an Agent powered by jiuwen-lite Agent Framework\n====================\n" << std::flush;

    auto& rm = ResourceManager::GetInstance();

    rm.RegisterTool("notebook_edit", []() { return std::make_unique<jiuwenClaw::NotebookEditTool>(); });
    rm.RegisterTool("cron", []() { return std::make_unique<jiuwenClaw::CronTool>(); });
    rm.RegisterTool("notify", []() { return std::make_unique<jiuwenClaw::NotifyTool>(); });

    std::cout << "\nInitializing Amap MCP Server (Streamable HTTP)...\n" << std::flush;
    try {
        std::string amapJson = R"({
            "url": "https://mcp.amap.com",
            "endpoint": "/mcp?key=<amap api key>",
            "isActive": "true",
            "description": "this is a mcp map server",
            "type": "streamable-http-client"
        })";

        rm.RegisterMCPServer("amap", amapJson);

        std::cout << "[SUCCESS] Amap MCP server connected. Available tools:\n" << std::flush;
        auto mcpTools = rm.GetAvailableTools();
        for (const auto& toolName : mcpTools) {
            std::cout << "  - " << toolName << "\n" << std::flush;
        }
    } catch (const std::exception& e) {
        std::cout << "[WARN] MCP Server initialization failed: " << e.what() << "\n" << std::flush;
    }

    AgentConfig config;
    config.id = "demo-agent";
    config.name = "Demo Agent";
    config.mode = AgentWorkMode::REACT;
    config.maxIterations = 50;

    config.contextConfig.sessionId = "session_01";
    config.contextConfig.storageType = ContextConfig::StorageType::MARKDOWN_FILE;
    config.contextConfig.storagePath = "./data/context";

    config.skillDirectory = "./my_skills";

    config.modelConfig.baseUrl = "<model server url>";
    config.modelConfig.apiKey = "<your api key>";
    config.modelConfig.modelName = "Qwen3.6-Plus";
    config.modelConfig.formatType = ModelFormatType::OPENAI;

    config.modelConfig.extraParams.Set("max_tokens", 4096);
    config.modelConfig.extraParams.Set("temperature", 0.0f);
    config.modelConfig.extraParams.Set("top_p", 0.0f);
    config.modelConfig.extraParams.Set("tool_choice", std::string("auto"));

    config.promptTemplates["react_system"] = PromptResource{
        PromptResourceType::FILE_PATH,
        "./examples/jiuwenClaw/templates/REACT_SYSTEM.md"
    };

    config.promptTemplates["agents"] = PromptResource{
        PromptResourceType::FILE_PATH,
        "./examples/jiuwenClaw/templates/AGENTS.md"
    };
    config.promptTemplates["soul"] = PromptResource{
        PromptResourceType::FILE_PATH,
        "./examples/jiuwenClaw/templates/SOUL.md"
    };
    config.promptTemplates["user"] = PromptResource{
        PromptResourceType::FILE_PATH,
        "./examples/jiuwenClaw/templates/USER.md"
    };
    config.promptTemplates["tools_md"] = PromptResource{
        PromptResourceType::FILE_PATH,
        "./examples/jiuwenClaw/templates/TOOLS.md"
    };

    Agent agent(config);

    jiuwenClaw::HeartbeatManager heartbeat(
        "./data/HEARTBEAT.md",
        agent,
        config.modelConfig,
        300
    );

    jiuwenClaw::CronWatcher cronWatcher("./data", agent, config.modelConfig, 60);

    auto allTools = rm.GetAvailableTools();
    agent.AddTools(allTools);

    std::cout << "\nEnter '/exit' to quit.\n" << std::flush;
    std::string query;
    while (true) {
        std::cout << "> " << std::flush;
        std::getline(std::cin, query);

        if (query == "/exit") {
            break;
        }

        if (query.empty()) {
            continue;
        }

        query = jiuwenClaw::LocalToUTF8(query);
        query = jiuwenClaw::CleanInput(query);
        query = jiuwenClaw::FixUTF8(query);

        size_t start = query.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) {
            continue;
        }
        size_t end = query.find_last_not_of(" \t\r\n");
        query = query.substr(start, end - start + 1);

        if (query.empty()) {
            continue;
        }

        std::cout << "Processing...\n";

        std::string utf8Carry;

        std::string fullResponse = agent.Invoke(query, [&](const std::string& resp) {
            std::string s = resp;

            std::vector<std::string> streamTags = {"[STREAM] ", "[STREAM]"};
            for (const auto& st : streamTags) {
                size_t p = s.find(st);
                while (p != std::string::npos) {
                    s.erase(p, st.length());
                    p = s.find(st, p);
                }
            }

            std::vector<std::string> hiddenTags = {"[STATUS]", "[THOUGHT]", "[ACTION]",
                                                   "[TOOL_CALLS]", "[TOOL_RESPONSE]",
                                                   "[RESPONSE]", "[FINAL]", "[ERROR]"};
            for (const auto& tag : hiddenTags) {
                size_t pos = s.find(tag);
                while (pos != std::string::npos) {
                    size_t contentStart = s.find('{', pos);
                    size_t endPos = std::string::npos;

                    if (contentStart != std::string::npos && contentStart < pos + tag.length() + 2) {
                        int depth = 1;
                        for (size_t i = contentStart + 1; i < s.length() && depth > 0; i++) {
                            if (s[i] == '{') depth++;
                            else if (s[i] == '}') depth--;
                            if (depth == 0) endPos = i + 1;
                        }
                    }

                    if (endPos == std::string::npos) {
                        size_t nextTagPos = std::string::npos;
                        for (const auto& ht : hiddenTags) {
                            size_t p2 = s.find(ht, pos + tag.length());
                            if (p2 != std::string::npos && (nextTagPos == std::string::npos || p2 < nextTagPos)) {
                                nextTagPos = p2;
                            }
                        }
                        endPos = (nextTagPos != std::string::npos) ? nextTagPos : s.length();
                    }

                    s.erase(pos, endPos - pos);
                    pos = s.find(tag);
                }
            }

            if (!s.empty()) {
                std::string fixed = jiuwenClaw::FixUTF8Streaming(s, utf8Carry);
                std::cout << jiuwenClaw::UTF8ToLocal(fixed) << std::flush;
            }
        });
        std::cout << "\n";
    }

    return 0;
}

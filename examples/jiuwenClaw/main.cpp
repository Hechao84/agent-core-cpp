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
#include "examples/jiuwenClaw/utils/logger.h"
#include "third_party/include/nlohmann/json.hpp"

using namespace jiuwen;
using namespace jiuwenClaw;

static std::string TrimStr(const std::string& str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

int main()
{
    std::cout << "jiuwenClaw is an Agent powered by jiuwen-lite Agent Framework\n====================\n" << std::flush;

    auto& rm = ResourceManager::GetInstance();

    rm.RegisterTool("notebook_edit", []() { return std::make_unique<NotebookEditTool>(); });
    rm.RegisterTool("cron", []() { return std::make_unique<CronTool>(); });
    rm.RegisterTool("notify", []() { return std::make_unique<NotifyTool>(); });

    std::cout << "\nInitializing Amap MCP Server (Streamable HTTP)...\n" << std::flush;
    try {
        std::string amapJson = R"({
            "url": "https://mcp.amap.com",
            "endpoint": "/mcp?key=<your amap key>",
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

    config.modelConfig.baseUrl = "<your model baseUrl>";
    config.modelConfig.apiKey = "<your model apiKey>";
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

    HeartbeatManager heartbeat(
        "./data/HEARTBEAT.md",
        agent,
        config.modelConfig,
        300
    );

    CronWatcher cronWatcher("./data", agent, config.modelConfig, 60);

    auto allTools = rm.GetAvailableTools();
    agent.AddTools(allTools);

    std::cout << "\nEnter '/exit' to quit.\n" << std::flush;
    std::string query;
    while (true) {
        std::cout << "> " << std::flush;
        std::getline(std::cin, query);

        if (query == "/exit") {
            LOG(INFO) << "User input: " << query;
            break;
        }

        if (query.empty()) {
            continue;
        }

        query = LocalToUTF8(query);
        query = CleanInput(query);
        query = FixUTF8(query);

        query = TrimStr(query);
        if (query.empty()) {
            continue;
        }

        std::cout << "Processing...\n";

        std::string utf8Carry;

        const std::string TAG_STREAM = "[STREAM]";
        const std::string TAG_STATUS = "[STATUS]";
        const std::string TAG_TOOL_CALLS = "[TOOL_CALLS]";
        const std::string TAG_TOOL_RESPONSE = "[TOOL_RESPONSE]";
        const std::string TAG_FINAL = "[FINAL]";

        std::string fullResponse = agent.Invoke(query, [&](const std::string& resp) {
            if (resp.empty()) return;

            std::string s = resp;
            // 1. 工具调用，内容与流式输出重复，不回显
            if (s.find(TAG_TOOL_CALLS) != std::string::npos) return;

            // 2. 工具响应
            if (s.find(TAG_TOOL_RESPONSE) != std::string::npos) {
                size_t start_pos = s.find(TAG_TOOL_RESPONSE);
                if (start_pos != std::string::npos) {
                    start_pos += TAG_TOOL_RESPONSE.length();
                    while (start_pos < s.length() && s[start_pos] == ' ') {
                        ++start_pos;
                    }
                    std::string toolRespText = s.substr(start_pos);
                    // 格式化工具响应输出
                    std::cout << "\n\n**Tool Response**: \n" << UTF8ToLocal(toolRespText) << std::endl;
                }
                return;
            }

            // 3. 流式输出
            if (s.find(TAG_STREAM) != std::string::npos) {
                size_t start_pos = s.find(TAG_STREAM);
                if (start_pos != std::string::npos) {
                    start_pos += TAG_STREAM.length();
                    while (start_pos < s.length() && s[start_pos] == ' ') {
                        ++start_pos;
                    }
                    std::string cleanText = s.substr(start_pos);

                    if (!cleanText.empty()) {
                        std::string fixed = FixUTF8Streaming(cleanText, utf8Carry);
                        std::cout << UTF8ToLocal(fixed) << std::flush;
                    }
                }
                return;
            }

            // 4. 状态信息
            if (s.find(TAG_STATUS) != std::string::npos) {
                size_t start_pos = s.find(TAG_STATUS);
                if (start_pos != std::string::npos) {
                    start_pos += TAG_STATUS.length();
                    while (start_pos < s.length() && s[start_pos] == ' ') {
                        ++start_pos;
                    }
                    std::string statusText = s.substr(start_pos);
                    std::string formattedStatus = "\n>> " + TrimStr(statusText) + "\n";
                    std::cout << UTF8ToLocal(formattedStatus) << std::flush;
                }
                return;
            }

            // 5. [FINAL] 标签通常表示结束，不做特殊处理
            if (s.find(TAG_FINAL) != std::string::npos) return;

            // 6. 兜底情况：无标签的纯文本
            if (!s.empty()) {
                std::cout << UTF8ToLocal(s) << std::flush;
            }

        });
        std::cout << "\n";
    }

    return 0;
}

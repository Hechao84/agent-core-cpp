#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "include/agent.h"
#include "include/resource_manager.h"
// Demo-specific tools (not part of the framework)
#include "tools/notebook_edit_tool.h"
#include "tools/file_state_tool.h"
#include "tools/cron_tool.h"
#ifdef _WIN32
    #include <windows.h>
#else
    // Linux/Unix specific alternatives if needed, or simply exclude logic
#endif
#include "src/3rd-party/include/nlohmann/json.hpp"

// Helper function to validate and fix UTF-8 encoding
// Replaces invalid UTF-8 bytes with the Unicode replacement character (U+FFFD)
std::string FixUTF8(const std::string& str)
{
    std::string result;
    size_t i = 0;
    while (i < str.length()) {
        unsigned char c = static_cast<unsigned char>(str[i]);
        int expectedLength = 0;
        
        if ((c & 0x80) == 0) {
            expectedLength = 1;
        } else if ((c & 0xE0) == 0xC0) {
            expectedLength = 2;
        } else if ((c & 0xF0) == 0xE0) {
            expectedLength = 3;
        } else if ((c & 0xF8) == 0xF0) {
            expectedLength = 4;
        } else {
            result += "\xEF\xBF\xBD";
            i++;
            continue;
        }
        
        if (i + expectedLength > str.length()) {
            result += "\xEF\xBF\xBD";
            break;
        }
        
        bool valid = true;
        for (int j = 1; j < expectedLength; ++j) {
            if ((str[i + j] & 0xC0) != 0x80) {
                valid = false;
                break;
            }
        }
        
        if (valid) {
            result.append(str.substr(i, expectedLength));
        } else {
            result += "\xEF\xBF\xBD";
        }
        i += expectedLength;
    }
    return result;
}

//Helper function to convert local encoding (GBK/ACP) to UTF-8 (Input)
std::string LocalToUTF8(const std::string& str)
{
#ifdef _WIN32
    try {
        int wlen = MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, NULL, 0);
        if (wlen <= 0) return str;
        std::wstring wstr(wlen, 0);
        MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, &wstr[0], wlen);
        
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
        if (len <= 0) return str;
        std::string result(len, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], len, NULL, NULL);
        if (!result.empty()) result.pop_back(); // Remove null terminator
        return result;
    } catch (...) {
        return str;
    }
#else
    return str;
#endif
}

// Helper function to convert UTF-8 to local encoding (GBK/ACP) (Output)
std::string UTF8ToLocal(const std::string& str)
{
#ifdef _WIN32
    try {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
        if (wlen <= 0) return str;
        std::wstring wstr(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], wlen);
        
        int len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
        if (len <= 0) return str;
        std::string result(len, 0);
        WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, &result[0], len, NULL, NULL);
        if (!result.empty()) result.pop_back(); // Remove null terminator
        return result;
    } catch (...) {
        return str;
    }
#else
    return str;
#endif
}

int main()
{
    std::cout << "Agent Framework Demo\n====================\n" << std::flush;

    auto& rm = ResourceManager::GetInstance();

    // Register demo-specific tools (not part of the framework)
    rm.RegisterTool("notebook_edit", []() { return std::make_unique<NotebookEditTool>(); });
    rm.RegisterTool("file_state", []() { return std::make_unique<FileStateTool>(); });
    rm.RegisterTool("cron", []() { return std::make_unique<CronTool>(); });

    // --- MCP Server Verification: Amap MCP Server ---
    // Using Streamable HTTP transport to connect to Amap's official hosted MCP server.
    // Replace <your amap key> with your actual API key.
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
    // ------------------------------------------------

    AgentConfig config;
    config.id = "demo-agent";
    config.name = "Demo Agent";
    config.mode = AgentWorkMode::REACT;
    config.maxIterations = 5;

    // 1. Context Config: Use Markdown file storage
    config.contextConfig.sessionId = "session_01";
    config.contextConfig.storageType = ContextConfig::StorageType::MARKDOWN_FILE;
    config.contextConfig.storagePath = "./data/context";
    
    // 2. Skill Config: Enable Skills from directory
    config.skillDirectory = "./my_skills"; // Relative path for demo purposes

    // 3. Model Config
    config.modelConfig.baseUrl = "<your llm endpoint>/v1";
    config.modelConfig.apiKey = "<your api key>";
    config.modelConfig.modelName = "Qwen3.6-Plus";
    config.modelConfig.formatType = ModelFormatType::OPENAI;
    
    // 4. Extended Model Params (ConfigNode)
    config.modelConfig.extraParams.Set("max_tokens", 4096);
    config.modelConfig.extraParams.Set("temperature", 0.0f);
    config.modelConfig.extraParams.Set("top_p", 0.0f);
    config.modelConfig.extraParams.Set("tool_choice", std::string("auto"));
    
    // Configure Prompt Templates using file references to jiuwenClaw config/templates directory
    config.promptTemplates["react_system"] = PromptResource{
        PromptResourceType::FILE_PATH,
        "./examples/jiuwenClaw/templates/REACT_SYSTEM.md"
    };
    
    // Bootstrap configuration files for Jiuwen Claw
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
    config.promptTemplates["heartbeat"] = PromptResource{
        PromptResourceType::FILE_PATH,
        "./examples/jiuwenClaw/templates/HEARTBEAT.md"
    };
           
    Agent agent(config);
    
    // Register all available tools (including MCP and demo-specific)
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
        
        query = LocalToUTF8(query);
        query = FixUTF8(query);
        
        // Remove backspace control characters (0x08 and 0x7F)
        std::string cleaned;
        for (char c : query) {
            if (c != '\b' && c != '\x7f') {
                cleaned += c;
            }
        }
        query = cleaned;
        
        // Trim whitespace
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
        
        bool is_streaming = false;
        agent.Invoke(query, [&](const std::string& resp) {
            std::string s = resp;
            std::vector<std::string> streamTags = {"[STREAM] ", "[STREAM]"};
            bool foundStream = false;
            for (const auto& st : streamTags) {
                size_t p = s.find(st);
                if (p != std::string::npos) foundStream = true;
                while (p != std::string::npos) {
                    s.erase(p, st.length());
                    p = s.find(st, p);
                }
            }
            if (foundStream) is_streaming = true;

            std::vector<std::string> controlTags = {"[STATUS]", "[THOUGHT]", "[ACTION]", "[TOOL_CALLS]", "[TOOL_RESPONSE]", "[RESPONSE]", "[FINAL]", "[ERROR]"};
            for (const auto& tag : controlTags) {
                size_t pos = s.find(tag);
                while (pos != std::string::npos) {
                    is_streaming = false;
                    if (pos > 0 && s[pos-1] != '\n') {
                        s.insert(pos, "\n");
                        pos += tag.length() + 1;
                    } else {
                        pos += tag.length();
                    }
                    pos = s.find(tag, pos);
                }
            }
            std::cout << UTF8ToLocal(FixUTF8(s)) << std::flush;
        });
        std::cout << "\n";
    }
    
    return 0;
}

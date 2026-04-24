#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "include/agent.h"
#include "include/resource_manager.h"
// Demo-specific tools
#include "tools/notebook_edit_tool.h"
#include "tools/cron_tool.h"
#include "tools/notify_tool.h"
// Heartbeat management module
#include "heartbeat_manager.h"
#include "cron_watcher.h"

#ifdef _WIN32
    #include <windows.h>
#else
    // Linux/Unix specific alternatives if needed, or simply exclude logic
#endif
#include "src/3rd-party/include/nlohmann/json.hpp"

// Helper function to sanitize user input.
// 1. Simulates Backspace (deletes previous UTF-8 char).
// 2. Strips ANSI Escapes (Arrows, Home, End).
// 3. Filters non-printable control characters.
std::string CleanInput(const std::string& input)
{
    std::string buffer;
    buffer.reserve(input.length());

    for (size_t i = 0; i < input.length(); ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);

        // 1. Handle Backspace (0x08) and DEL (0x7F)
        // Simulates the terminal backspace behavior: remove the last character added
        if (c == 0x08 || c == 0x7F) {
            if (!buffer.empty()) {
                // Find the start of the last UTF-8 character to delete it properly
                size_t idx = buffer.length() - 1;
                // Scan backwards: UTF-8 continuation bytes start with 10xxxxxx
                while (idx > 0 && (buffer[idx] & 0xC0) == 0x80) {
                    idx--;
                }
                // Erase from the start of the char to the end
                buffer.erase(idx);
            }
            continue;
        }

        // 2. Skip ANSI Escape Sequences (Arrows, etc.)
        // Standard sequence: ESC [ ...
        if (c == 0x1B) {
            if (i + 1 < input.length() && input[i+1] == '[') {
                i++; // Skip '['
                // Continue skipping until we hit the command byte (A-Z, a-z, @)
                while (i + 1 < input.length()) {
                    i++;
                    unsigned char next = static_cast<unsigned char>(input[i]);
                    // Command bytes are typically letters
                    if ((next >= 'A' && next <= 'z') || next == '@') break;
                }
            }
            continue;
        }

        // 3. Skip other control characters (< 0x20) except valid whitespace
        if (c < 0x20 && c != '\r' && c != '\n' && c != '\t') {
            continue;
        }

        // 4. Append valid printable characters
        buffer += static_cast<char>(c);
    }

    return buffer;
}

// Helper function to validate and fix UTF-8 encoding
// Modified to SILENTLY SKIP invalid bytes to avoid garbage symbols in logs
std::string FixUTF8(const std::string& str)
{
    std::string result;
    result.reserve(str.length());
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
            // Invalid leading byte -> Skip silently (don't add replacement char)
            i++;
            continue;
        }
        
        if (i + expectedLength > str.length()) {
            break; // Truncated char at end -> Stop
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
            i += expectedLength;
        } else {
            // Invalid continuation -> Skip this leading byte, try next
            i++;
        }
    }
    return result;
}

// Stateful UTF-8 fixer for streaming output.
// Accumulates incomplete trailing UTF-8 bytes and carries them over to the next chunk.
std::string FixUTF8Streaming(const std::string& str, std::string& carryBuffer)
{
    std::string fullInput = carryBuffer + str;
    carryBuffer.clear();
    
    std::string result;
    size_t i = 0;
    while (i < fullInput.length()) {
        unsigned char c = static_cast<unsigned char>(fullInput[i]);
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
        
        if (i + expectedLength > fullInput.length()) {
            carryBuffer = fullInput.substr(i);
            break;
        }
        
        bool valid = true;
        for (int j = 1; j < expectedLength; ++j) {
            if ((fullInput[i + j] & 0xC0) != 0x80) {
                valid = false;
                break;
            }
        }
        
        if (valid) {
            result.append(fullInput.substr(i, expectedLength));
        } else {
            result += "\xEF\xBF\xBD";
        }
        i += expectedLength;
    }
    return result;
}

// Helper function to convert local encoding (GBK/ACP) to UTF-8 (Input)
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
        if (!result.empty()) result.pop_back();
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
        if (!result.empty()) result.pop_back();
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
    std::cout << "jiuwenClaw is an Agent powered by jiuwen-lite Agent Framework\n====================\n" << std::flush;

    auto& rm = ResourceManager::GetInstance();

    // --- Register Tools ---
    
    // Register demo-specific tools (not part of the framework)
    rm.RegisterTool("notebook_edit", []() { return std::make_unique<NotebookEditTool>(); });
    rm.RegisterTool("cron", []() { return std::make_unique<CronTool>(); });
    rm.RegisterTool("notify", []() { return std::make_unique<NotifyTool>(); });

    // --- MCP Server Verification: Amap MCP Server ---
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

    // 1. Context Config
    config.contextConfig.sessionId = "session_01";
    config.contextConfig.storageType = ContextConfig::StorageType::MARKDOWN_FILE;
    config.contextConfig.storagePath = "./data/context";
    
    // 2. Skill Config
    config.skillDirectory = "./my_skills";

    // 3. Model Config
    config.modelConfig.baseUrl = "<your llm endpoint>/v1";
    config.modelConfig.apiKey = "<your api key>";
    config.modelConfig.modelName = "Qwen3.6-Plus";
    config.modelConfig.formatType = ModelFormatType::OPENAI;
    
    // 4. Extended Model Params
    config.modelConfig.extraParams.Set("max_tokens", 4096);
    config.modelConfig.extraParams.Set("temperature", 0.0f);
    config.modelConfig.extraParams.Set("top_p", 0.0f);
    config.modelConfig.extraParams.Set("tool_choice", std::string("auto"));
    
    // Configure Prompt Templates
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
            break;
        }
        
        if (query.empty()) {
            continue;
        }
                
        query = LocalToUTF8(query);
        query = CleanInput(query);
        query = FixUTF8(query);

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
                std::string fixed = FixUTF8Streaming(s, utf8Carry);
                std::cout << UTF8ToLocal(fixed) << std::flush;
            }
        });
        std::cout << "\n";
    }
    
    return 0;
}

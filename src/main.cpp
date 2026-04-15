#include <iostream>
#include <string>
#include "agent.h"
#include "resource_manager.h"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

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

class WeatherTool : public Tool {
    public:
        WeatherTool() : Tool("weather", "Get weather information for a city", 
            {{"city", "The city name", "string", true}}) {}
        std::string Invoke(const std::string& input) override
        {
            return "Weather in " + input + ": Sunny, 25°C";
        }
};

int main()
{
    std::cout << "Agent Framework Demo\n====================\n" << std::flush;

    // Disable proxy for HTTP requests to external services
    unsetenv("https_proxy");
    unsetenv("HTTPS_PROXY");
    unsetenv("http_proxy");
    unsetenv("HTTP_PROXY");
    unsetenv("no_proxy");
    unsetenv("NO_PROXY");

    auto& rm = ResourceManager::GetInstance();

    // Register a local weather tool for demonstration
    rm.RegisterTool("weather", []() { return std::make_unique<WeatherTool>(); });

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
    config.modelConfig.baseUrl = "<your base url>";
    config.modelConfig.apiKey = "<your api key>";
    config.modelConfig.modelName = "<your model name>";
    config.modelConfig.formatType = ModelFormatType::OPENAI;
    
    // 4. Extended Model Params (ConfigNode)
    config.modelConfig.extraParams.Set("max_tokens", 2048);
    config.modelConfig.extraParams.Set("temperature", 0.0f);
    config.modelConfig.extraParams.Set("top_p", 0.0f);
    config.modelConfig.extraParams.Set("tool_choice", std::string("auto"));
    
    config.promptTemplates["react_system"] = 
        "You are a reasoning agent. You must reply in the same language as the user's query.\nSkills:\n{skills}\nTools:\n{tools}\nQuestion: {query}\n{context}";
    
    Agent agent(config);
    // Add local weather tool and let the agent discover MCP tools via RM automatically
    agent.AddTools({"weather"});
    
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
            std::cout << UTF8ToLocal(s) << std::flush;
        });
        std::cout << "\n";
    }
    
    return 0;
}

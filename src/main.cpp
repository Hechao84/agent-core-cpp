#include <iostream>
#include <string>
#include "agent.h"
#include "resource_manager.h"

#ifdef _WIN32
#include <windows.h>
#endif

// Helper function to convert local encoding (GBK/ACP) to UTF-8 (Input)
std::string LocalToUTF8(const std::string& str) {
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
std::string UTF8ToLocal(const std::string& str) {
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
        std::string Invoke(const std::string& input) override {
            return "Weather in " + input + ": Sunny, 25°C";
        }
};

int main() {
    std::cout << "Agent Framework Demo\n====================\n" << std::flush;
    std::cout << "Enter '/exit' to quit.\n" << std::flush;
    
    auto& rm = ResourceManager::GetInstance();
    rm.RegisterTool("weather", []() { return std::make_unique<WeatherTool>(); });
    
    AgentConfig config;
    config.id = "demo-agent";
    config.name = "Demo Agent";
    config.mode = AgentWorkMode::REACT;
    config.maxIterations = 5;

    // 1. Context Config: Enable SQLite persistence
    config.contextConfig.sessionId = "session_01";
    config.contextConfig.storageType = ContextConfig::StorageType::DATABASE;
    
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
    agent.AddTools({"weather"});
    
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
        
        // Convert input to UTF-8
        query = LocalToUTF8(query);
        
        std::cout << "Processing...\n";
        agent.Invoke(query, [](const std::string& resp) {
            // Convert response from UTF-8 to Local (GBK) for display on Windows
            std::string outStr = UTF8ToLocal(resp);
            std::cout << outStr << std::endl; // Use println as resp might not have newline
        });
        std::cout << "\n";
    }
    
    return 0;
}

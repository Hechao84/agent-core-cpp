
#include <iostream>
#include <string>
#include "include/agent.h"
#include "include/resource_manager.h"
#ifdef _WIN32
    #include <windows.h>
#else
    // Linux/Unix specific alternatives if needed, or simply exclude logic
#endif

#ifdef _WIN32
#endif

// Helper function to convert local encoding to UTF-8 (Input for Agent)
// Crucial for Windows console Chinese input support
std::string LocalToUTF8(const std::string& str)
{
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, NULL, 0);
    if (wlen <= 0) return str;
    std::wstring wstr(wlen, 0);
    MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, &wstr[0], wlen);
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
    if (len <= 0) return str;
    std::string res(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &res[0], len, NULL, NULL);
    if (!res.empty()) res.pop_back();
    return res;
#endif
    return str;
}

void RunTestQuery(Agent& agent, const std::string& query)
{
    std::cout << "\n========================================\n"
              << "[TEST] Query: " << query << "\n"
              << "========================================" << std::endl;
              
    std::string utf8Query = LocalToUTF8(query);

    agent.Invoke(utf8Query, [](const std::string& resp)
    {
        bool isStream = (resp.find("[STREAM]") == 0);
        if (isStream) {
            std::string content = resp;
            if (content.find("[STREAM] ") == 0) content = content.substr(9);
            else if (content.find("[STREAM]") == 0) content = content.substr(8);
            std::cout << content << std::flush;
        } else {
            std::cout << "\n" << resp << std::endl;
        }
    });
    std::cout << "\n" << std::endl;
}

int main()
{
    std::cout << "=== Running Testcases for Builtin Tools & Skills ===\n" << std::flush;
    std::cout << "[INFO] Checking Skills directory: ./my_skills\n" << std::flush;

    // 2. Configuration
    AgentConfig config;
    config.id = "test-agent";
    config.name = "Test Agent";
    config.mode = AgentWorkMode::REACT;
    config.maxIterations = 5;

    // Model Config
    config.modelConfig.baseUrl = "<your base url>";
    config.modelConfig.apiKey = "<your api key>";
    config.modelConfig.modelName = "<your model name>";
    config.modelConfig.formatType = ModelFormatType::OPENAI;
    config.modelConfig.extraParams.Set("max_tokens", 4096);

    // Context Config
    config.contextConfig.sessionId = "test_session";
    config.contextConfig.storageType = ContextConfig::StorageType::MARKDOWN_FILE;
    config.contextConfig.storagePath = "./data/context_test_builtin";
    
    // Skill Config
    config.skillDirectory = "./my_skills";

    // Prompt Template
    config.promptTemplates["react_system"] = 
        R"(You are a helpful assistant with access to tools and skills.
Skills:
{skills}
Tools:
{tools}
Context:
{context}
Question: {query}
You must reply in the same language as the user's query.)";

    // 3. Initialize Agent
    Agent agent(config);
    agent.AddTools({"time_info", "web_search", "web_fetcher"});

    // 4. Run Testcases
    RunTestQuery(agent, "What is the current time?");
    RunTestQuery(agent, "Can you search for 'NVIDIA RTX 5090 specs'?");
    RunTestQuery(agent, "Fetch the content of https://httpbin.org/get");

    return 0;
}

#include <iostream>
#include "agent.h"
#include "resource_manager.h"

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
    
    auto& rm = ResourceManager::GetInstance();
    rm.RegisterTool("weather", []() { return std::make_unique<WeatherTool>(); });
    
    AgentConfig config;
    config.id = "demo-agent";
    config.name = "Demo Agent";
    config.mode = AgentWorkMode::REACT;
    config.maxIterations = 5;
    
    // 配置模型参数
    config.modelConfig.baseUrl = "<your base url>";
    config.modelConfig.apiKey = "<your api key>";
    config.modelConfig.modelName = "<your model name>";
    config.modelConfig.formatType = ModelFormatType::OPENAI;
    
    config.promptTemplates["react_system"] = 
        "You are a reasoning agent.\nTools:\n{tools}\nQuestion: {query}\n{context}";
    
    Agent agent(config);
    agent.AddTools({"weather"});
    
    std::cout << "\nQuery: What's the weather in Beijing?\n";
    agent.Invoke("What's the weather in Beijing?", [](const std::string& resp) {
        std::cout << resp << std::endl;
    });
    
    return 0;
}

#pragma once
#include <string>
#include <map>
#include <any>
#include <unordered_map>
#include <vector>

enum class ModelFormatType {
    OPENAI,
    ANTHROPIC,
    DEEPSEEK,
    DASHSCOPE,
};

struct ModelConfig {
    std::string baseUrl;
    std::string apiKey;
    std::string modelName;
    std::map<std::string, std::any> configs;
    ModelFormatType formatType{ModelFormatType::OPENAI};
};

enum class AgentWorkMode {
    REACT,
    PLAN_AND_EXECUTE,
    WORKFLOW,
};

struct AgentConfig {
    std::string id;
    std::string name;
    std::string description;
    std::string version;
    AgentWorkMode mode;
    ModelConfig modelConfig;
    std::unordered_map<std::string, std::string> promptTemplates;
    std::string skillDirectory;
    int maxIterations{10};
};

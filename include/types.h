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

// Context Engine Configuration
struct ContextConfig {
    int maxContextTokens{4096};
    std::string sessionId;
    std::string storagePath; // For DB: path to file, For File: directory path
    enum class StorageType { MEMORY_ONLY, MARKDOWN_FILE, DATABASE };
    StorageType storageType{StorageType::DATABASE}; // Default to DB (SQLite) for persistence
};

struct AgentConfig {
    std::string id;
    std::string name;
    std::string description;
    std::string version;
    AgentWorkMode mode;
    ModelConfig modelConfig;
    std::unordered_map<std::string, std::string> promptTemplates;
    ContextConfig contextConfig; // <-- Add Context Configuration
    std::string skillDirectory;
    int maxIterations{10};
};

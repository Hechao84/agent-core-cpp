#pragma once
#include <string>
#include <vector>
#include <variant>
#include <memory>
#include <map>
#include <unordered_map>

// Forward declaration for recursive configuration structure
struct ConfigNode;

// ConfigValue: A type-safe variant that supports primitives, lists, and nested nodes.
// Replaces std::any to ensure static type safety while supporting hierarchical configuration.
using ConfigValue = std::variant<
    int,
    float,
    bool,
    std::string,
    std::vector<std::string>,
    std::shared_ptr<ConfigNode> // Recursive pointer allows for nested hierarchy
>;

// ConfigNode: Represents a branch or leaf map in the configuration tree
struct ConfigNode {
    std::map<std::string, ConfigValue> fields_;

    // Set a value (overwrites if exists)
    void Set(const std::string& key, ConfigValue value)
    {
        fields_[key] = std::move(value);
    }

    // Set a value using dot-notation path (e.g., "model.temperature") to support hierarchy
    void SetNested(const std::string& path, ConfigValue value)
    {
        size_t pos = path.find('.');
        if (pos == std::string::npos) {
            fields_[path] = std::move(value);
        } else {
            std::string key = path.substr(0, pos);
            std::string rest = path.substr(pos + 1);

            std::shared_ptr<ConfigNode> node;
            auto it = fields_.find(key);
            if (it != fields_.end()) {
                // Try to cast existing value to Node
                if (auto p = std::get_if<std::shared_ptr<ConfigNode>>(&it->second)) {
                    node = *p;
                }
            }

            // Create node if not exists
            if (!node) {
                node = std::make_shared<ConfigNode>();
                fields_[key] = node;
            }

            node->SetNested(rest, std::move(value));
        }
    }

    // Get pointer to value (returns nullptr if not found or type mismatch)
    template <typename T>
    const T* GetPtr(const std::string& key) const
    {
        auto it = fields_.find(key);
        if (it != fields_.end()) {
            return std::get_if<T>(&(it->second));
        }
        return nullptr;
    }

    // Convenience getter with default value
    template <typename T>
    T GetValue(const std::string& key, T defaultVal) const
    {
        if (const T* val = GetPtr<T>(key)) {
            return *val;
        }
        return defaultVal;
    }
};

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
    ModelFormatType formatType{ModelFormatType::OPENAI};

    // Extended parameters supporting hierarchy (e.g., "model.temperature")
    // Uses std::variant instead of std::any for type safety.
    ConfigNode extraParams;
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
    StorageType storageType{StorageType::MARKDOWN_FILE}; // Default to Markdown file for persistence
};

struct AgentConfig {
    std::string id;
    std::string name;
    std::string description;
    std::string version;
    AgentWorkMode mode;
    ModelConfig modelConfig;
    std::unordered_map<std::string, std::string> promptTemplates;
    ContextConfig contextConfig;
    std::string skillDirectory;
    int maxIterations{10};
};

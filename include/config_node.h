#pragma once

#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace jiuwen {

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
struct ConfigNode 
{
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

} // namespace jiuwen

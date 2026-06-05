#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "include/agent_export.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

struct ToolParam
{
    std::string name;
    std::string description;
    std::string type;
    bool required{false};
};

class AGENT_API Tool {
public:
    Tool(std::string name, std::string description, std::vector<ToolParam> params);
    virtual ~Tool() = default;
    virtual std::string Invoke(const std::string& input) = 0;
    std::string GetName() const;
    std::string GetDescription() const;
    std::vector<ToolParam> GetParams() const;
    std::string GetSchema() const;

    // Return a structured JSON Schema describing this tool's parameters:
    //   { "type":"object", "properties":{...}, "required":[...] }
    // Default implementation auto-builds from ToolParam list. Override only
    // when a tool needs schema features beyond ToolParam (enums, defaults,
    // nested objects).
    virtual nlohmann::json GetJsonSchema() const;
protected:
    std::string name_;
    std::string description_;
    std::vector<ToolParam> params_;
};

} // namespace jiuwen

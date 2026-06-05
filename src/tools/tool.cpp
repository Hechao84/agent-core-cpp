#include "include/tool.h"

#include <sstream>
#include <string>
#include <vector>

#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

Tool::Tool(std::string name, std::string description, std::vector<ToolParam> params)
    : name_(std::move(name)), description_(std::move(description)), params_(std::move(params))
{
}

std::string Tool::GetName() const
{
    return name_;
}

std::string Tool::GetDescription() const
{
    return description_;
}

std::vector<ToolParam> Tool::GetParams() const
{
    return params_;
}

std::string Tool::GetSchema() const
{
    std::ostringstream oss;
    oss << "Tool: " << name_ << "\n";
    oss << "Description: " << description_ << "\n";
    oss << "Parameters:\n";
    for (const auto& param : params_) {
        oss << "  - " << param.name << " (" << param.type << ")";
        if (param.required) oss << " [required]";
        oss << ": " << param.description << "\n";
    }
    return oss.str();
}

nlohmann::json Tool::GetJsonSchema() const
{
    nlohmann::json schema;
    schema["type"] = "object";
    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json required = nlohmann::json::array();
    for (const auto& p : params_) {
        nlohmann::json prop;
        // Map our coarse type strings to JSON-Schema types.
        std::string t = p.type;
        if (t == "int" || t == "integer") {
            prop["type"] = "integer";
        } else if (t == "number" || t == "float" || t == "double") {
            prop["type"] = "number";
        } else if (t == "bool" || t == "boolean") {
            prop["type"] = "boolean";
        } else if (t == "array") {
            prop["type"] = "array";
            prop["items"] = {{"type", "string"}};
        } else if (t == "object") {
            prop["type"] = "object";
        } else {
            prop["type"] = "string";
        }
        if (!p.description.empty()) {
            prop["description"] = p.description;
        }
        properties[p.name] = prop;
        if (p.required) {
            required.push_back(p.name);
        }
    }
    schema["properties"] = properties;
    if (!required.empty()) {
        schema["required"] = required;
    }
    return schema;
}

} // namespace jiuwen

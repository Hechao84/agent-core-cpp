#include "tool.h"
#include <sstream>

Tool::Tool(std::string name, std::string description, std::vector<ToolParam> params)
    : name_(std::move(name)), description_(std::move(description)), params_(std::move(params)) {}

std::string Tool::GetName() const { return name_; }
std::string Tool::GetDescription() const { return description_; }
std::vector<ToolParam> Tool::GetParams() const { return params_; }

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

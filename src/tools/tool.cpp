#include "tool.h"
#include <sstream>

Tool::Tool(std::string name, std::string description, std::vector<ToolParam> params)
    : m_name(std::move(name)), m_description(std::move(description)), m_params(std::move(params)) {}

std::string Tool::GetName() const { return m_name; }
std::string Tool::GetDescription() const { return m_description; }
std::vector<ToolParam> Tool::GetParams() const { return m_params; }

std::string Tool::GetSchema() const {
    std::ostringstream oss;
    oss << "Tool: " << m_name << "\n";
    oss << "Description: " << m_description << "\n";
    oss << "Parameters:\n";
    for (const auto& param : m_params) {
        oss << "  - " << param.name << " (" << param.type << ")";
        if (param.required) oss << " [required]";
        oss << ": " << param.description << "\n";
    }
    return oss.str();
}

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "include/agent_export.h"

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
protected:
    std::string name_;
    std::string description_;
    std::vector<ToolParam> params_;
};

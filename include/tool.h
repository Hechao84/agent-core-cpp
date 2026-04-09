#pragma once
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <memory>

struct ToolParam {
    std::string name;
    std::string description;
    std::string type;
    bool required{false};
};

class Tool {
    public:
        Tool(std::string name, std::string description, std::vector<ToolParam> params);
        virtual ~Tool() = default;
        virtual std::string Invoke(const std::string& input) = 0;
        std::string GetName() const;
        std::string GetDescription() const;
        std::vector<ToolParam> GetParams() const;
        std::string GetSchema() const;
    protected:
        std::string m_name;
        std::string m_description;
        std::vector<ToolParam> m_params;
};

#pragma once
#include "tool.h"
#include <string>

class WebSearchTool : public Tool {
public:
    WebSearchTool();
    std::string Invoke(const std::string& input) override;
private:
    std::string Search(const std::string& query);
};

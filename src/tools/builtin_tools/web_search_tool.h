#pragma once

#include <string>
#include "include/tool.h"

class WebSearchTool : public Tool {
public:
    WebSearchTool();
    std::string Invoke(const std::string& input) override;
private:
    std::string Search(const std::string& query);
};

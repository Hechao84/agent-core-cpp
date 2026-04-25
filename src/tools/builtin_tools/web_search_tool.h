#pragma once

#include <string>
#include "include/tool.h"

namespace jiuwen {

class WebSearchTool : public Tool {
public:
    WebSearchTool();
    std::string Invoke(const std::string& input) override;
private:
    std::string Search(const std::string& query);
};

} // namespace jiuwen

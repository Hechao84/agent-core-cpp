#pragma once

#include <string>
#include "include/tool.h"

namespace jiuwen {

class TimeInfoTool : public Tool {
public:
    TimeInfoTool();
    std::string Invoke(const std::string& input) override;
};

} // namespace jiuwen

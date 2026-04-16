#pragma once

#include <string>
#include "include/tool.h"

class TimeInfoTool : public Tool {
public:
    TimeInfoTool();
    std::string Invoke(const std::string& input) override;
};

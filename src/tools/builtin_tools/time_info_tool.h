#pragma once
#include "tool.h"
#include <string>

class TimeInfoTool : public Tool {
public:
    TimeInfoTool();
    std::string Invoke(const std::string& input) override;
};

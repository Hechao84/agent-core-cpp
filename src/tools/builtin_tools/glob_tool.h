#pragma once
#include "tool.h"
#include <string>

class GlobTool : public Tool {
public:
    GlobTool();
    std::string Invoke(const std::string& input) override;
};

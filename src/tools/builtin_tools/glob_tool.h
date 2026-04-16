#pragma once

#include <string>
#include "include/tool.h"

class GlobTool : public Tool {
public:
    GlobTool();
    std::string Invoke(const std::string& input) override;
};

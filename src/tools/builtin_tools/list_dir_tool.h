#pragma once
#include "tool.h"
#include <string>

class ListDirTool : public Tool {
public:
    ListDirTool();
    std::string Invoke(const std::string& input) override;
};

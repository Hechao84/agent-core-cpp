#pragma once

#include <string>
#include "include/tool.h"

class ListDirTool : public Tool {
public:
    ListDirTool();
    std::string Invoke(const std::string& input) override;
};

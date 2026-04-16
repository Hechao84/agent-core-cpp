#pragma once

#include <string>
#include "include/tool.h"

class ReadFileTool : public Tool {
public:
    ReadFileTool();
    std::string Invoke(const std::string& input) override;
};

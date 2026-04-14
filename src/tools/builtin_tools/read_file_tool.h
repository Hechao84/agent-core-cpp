#pragma once
#include "tool.h"
#include <string>

class ReadFileTool : public Tool {
public:
    ReadFileTool();
    std::string Invoke(const std::string& input) override;
};

#pragma once
#include "tool.h"
#include <string>

class WriteFileTool : public Tool {
public:
    WriteFileTool();
    std::string Invoke(const std::string& input) override;
};

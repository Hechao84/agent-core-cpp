#pragma once

#include <string>
#include "include/tool.h"

class WriteFileTool : public Tool {
public:
    WriteFileTool();
    std::string Invoke(const std::string& input) override;
};

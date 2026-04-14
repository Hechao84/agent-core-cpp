#pragma once
#include "tool.h"
#include <string>

class EditFileTool : public Tool {
public:
    EditFileTool();
    std::string Invoke(const std::string& input) override;
};

#pragma once

#include <string>
#include "include/tool.h"

class EditFileTool : public Tool {
public:
    EditFileTool();
    std::string Invoke(const std::string& input) override;
};

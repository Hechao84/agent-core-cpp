#pragma once

#include <string>
#include "include/tool.h"

namespace jiuwen {

class ReadFileTool : public Tool {
public:
    ReadFileTool();
    std::string Invoke(const std::string& input) override;
};

} // namespace jiuwen

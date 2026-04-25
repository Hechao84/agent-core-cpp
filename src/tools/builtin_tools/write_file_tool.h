#pragma once

#include <string>
#include "include/tool.h"

namespace jiuwen {

class WriteFileTool : public Tool {
public:
    WriteFileTool();
    std::string Invoke(const std::string& input) override;
};

} // namespace jiuwen

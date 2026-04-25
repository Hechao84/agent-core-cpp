#pragma once

#include <string>
#include "include/tool.h"

namespace jiuwen {

class ListDirTool : public Tool {
public:
    ListDirTool();
    std::string Invoke(const std::string& input) override;
};

} // namespace jiuwen

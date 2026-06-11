#pragma once

#include <string>

#include "include/tool.h"

namespace jiuwen {

class MemoryRuntime;

class MemoryReadPayloadTool : public Tool
{
public:
    explicit MemoryReadPayloadTool(MemoryRuntime* memoryRuntime);
    std::string Invoke(const std::string& input) override;

private:
    MemoryRuntime* memoryRuntime_{nullptr};
};

} // namespace jiuwen

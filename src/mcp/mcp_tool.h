#pragma once

#include <memory>
#include <string>
#include <vector>

#include "include/tool.h"
#include "src/mcp/mcp_connection.h"

namespace jiuwen {

class MCPTool : public Tool
{
public:
    MCPTool(std::string name, std::string description, std::vector<ToolParam> params,
            std::shared_ptr<MCPConnection> server);
    std::string Invoke(const std::string& input) override;

private:
    std::shared_ptr<MCPConnection> server_;
};

} // namespace jiuwen

#include "src/mcp/mcp_tool.h"

#include <memory>
#include <string>
#include <vector>

namespace jiuwen {

MCPTool::MCPTool(std::string name, std::string description, std::vector<ToolParam> params,
                 std::shared_ptr<MCPConnection> server)
    : Tool(std::move(name), std::move(description), std::move(params)), server_(std::move(server))
{
}

std::string MCPTool::Invoke(const std::string& input)
{
    if (!server_ || !server_->IsConnected()) {
        return "Error: MCP Server not connected or invalid";
    }

    std::shared_ptr<MCPToolResult> result = server_->CallTool(name_, input);
    if (!result) {
        return "Error: MCP tool call returned null result";
    }

    std::string output;
    if (result->isError) {
        output = "MCP Tool Error: ";
    }
    for (const auto& content : result->content) {
        output += content;
    }
    return output;
}

} // namespace jiuwen

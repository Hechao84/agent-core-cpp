#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include "tool.h"

enum class MCPTransportType { STDIO, HTTP, SSE };

struct MCPEndpointConfig {
    std::string command;
    std::vector<std::string> args;
    std::string url;
    MCPTransportType transportType{MCPTransportType::STDIO};
    std::unordered_map<std::string, std::string> env;
    std::unordered_map<std::string, std::string> headers;
};

class MCPTool : public Tool {
    public:
        MCPTool(std::string name, std::string description, std::vector<ToolParam> params, std::weak_ptr<class MCPServer> server);
        std::string Invoke(const std::string& input) override;
    private:
        std::weak_ptr<class MCPServer> m_server;
};

class MCPServer : public std::enable_shared_from_this<MCPServer> {
    public:
        MCPServer(std::string name, MCPEndpointConfig config);
        void Connect();
        void Disconnect();
        std::vector<std::string> ListTools();
        std::unique_ptr<MCPTool> GetTool(const std::string& toolName);
        bool IsConnected() const;
        std::string GetName() const;
    private:
        std::string m_name;
        MCPEndpointConfig m_config;
        bool m_connected{false};
        std::vector<std::string> m_availableTools;
        std::vector<std::string> FetchToolsList();
};

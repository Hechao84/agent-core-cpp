# MCP 集成设计文档

## 1. 模块概述

MCP（Model Context Protocol）集成模块（`src/mcp/`）实现了与外部 MCP 服务器的交互，使 Agent 能够动态发现和调用远程工具。MCP 是一种标准化的工具发现和调用协议，支持多种传输方式，允许 Agent 无缝扩展其工具能力。

模块包含以下组件：

| 类 | 文件 | 职责 |
|----|------|------|
| `MCPClient` | `src/mcp/mcp_client.h` | JSON-RPC 协议客户端 |
| `MCPConnection` | `src/mcp/mcp_connection.h` | 服务器连接与生命周期管理 |
| `MCPTool` | `src/mcp/mcp_tool.h` | 远程工具的本地代理 |
| `MCPConfigManager` | `src/mcp/mcp_config_manager.h` | 运行时配置管理 |

## 2. MCPClient JSON-RPC 协议实现

### 2.1 设计意图

`MCPClient` 是 MCP 协议的底层通信实现，负责发送 JSON-RPC 请求和处理响应。它封装了协议细节，使上层组件不需要关心消息格式。

### 2.2 类结构

```
MCPClient
  │  ├── name_ (string)                ← 客户端名称（用于 initialize）
  │  ├── version_ (string)             ← 客户端版本
  │  ├── endpoint_ (string)            ← MCP 服务器端点 URL
  │  ├── sessionId_ (string)           ← 协议会话 ID（initialize 返回）
  │  ├── isInitialized_ (bool)         ← 是否完成初始化握手
  │  ├── nextRequestId_ (atomic<int>)  ← 请求 ID 计数器
  │  ├── headers_ (vector<string>)     ← HTTP 请求头
  │  ├── lastError_ (string)           ← 最近错误信息
  │  ├── sessionMutex_ (mutex)         ← 保护会话状态
  │  │
  │  ├── Initialize()                  ← 协议初始化握手
  │  ├── ListTools()                   ← 发现服务器提供的工具
  │  ├── CallTool(name, arguments)     ← 执行工具调用
  │  ├── SendRequest(request)          ← 发送 JSON-RPC 请求
  │  └── MakeErrorResponse(message)    ← 构造错误响应
```

### 2.3 协议流程

```
MCPClient 生命周期:
  │
  │  1. Initialize()
  │     ├── 发送 JSON-RPC "initialize" 请求:
  │     │   {"jsonrpc":"2.0","id":1,"method":"initialize",
  │     │    "params":{"clientInfo":{"name":"jiuwen-lite","version":"0.1"}}}
  │     ├── 接收响应 → 获取 sessionId
  │     ├── 发送 "initialized" 通知（无需响应）
  │     └── isInitialized_ = true
  │
  │  2. ListTools()
  │     ├── 发送 "tools/list" 请求:
  │     │   {"jsonrpc":"2.0","id":2,"method":"tools/list"}
  │     ├── 接收响应 → 解析为 vector<MCPToolInfo>
  │     └── 每个 MCPToolInfo: {name, description, inputSchema}
  │
  │  3. CallTool(toolName, arguments)
  │     ├── 发送 "tools/call" 请求:
  │     │   {"jsonrpc":"2.0","id":3,"method":"tools/call",
  │     │    "params":{"name":"weather","arguments":{"city":"Beijing"}}}
  │     ├── 接收响应 → 解析为 MCPToolResult
  │     └── MCPToolResult: {isError, content[]}
  │
  │  4. 后续请求使用递增的 nextRequestId_
```

### 2.4 JSON-RPC 规范

所有请求遵循 JSON-RPC 2.0 规范：

```json
// 请求
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": { ... }
}

// 成功响应
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": { ... }
}

// 错误响应
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": { "code": -32600, "message": "Invalid Request" }
}
```

### 2.5 线程安全

- `sessionMutex_` 保护 `sessionId_` 和 `isInitialized_` 的并发访问
- `nextRequestId_` 使用 `atomic<int>`，多线程递增安全
- `SendRequest` 内部串行化，确保同一连接的请求有序

## 3. MCPConnection 传输管理

### 3.1 设计意图

`MCPConnection` 管理单个 MCP 服务器的完整生命周期，包括连接、发现、断连和重连。它封装了传输层细节，提供统一的服务器交互接口。

### 3.2 类结构

```
MCPConnection (enable_shared_from_this)
  │  ├── name_ (string)              ← 服务器名称
  │  ├── config_ (MCPEndpointConfig) ← 传输配置
  │  ├── connected_ (bool)           ← 连接状态
  │  ├── availableTools_ (vector<MCPToolInfo>) ← 发现的工具列表
  │  ├── client_ (shared_ptr<MCPClient>) ← JSON-RPC 客户端
  │  ├── stateMutex_ (mutex)         ← 保护连接状态
  │  ├── callMutex_ (mutex)          ← 保护工具调用
  │  │
  │  ├── Connect()                   ← 建立连接并发现工具
  │  ├── Disconnect()                ← 断开连接
  │  ├── ListTools()                 ← 重新发现工具
  │  ├── GetTool(toolName)           ← 获取工具代理
  │  ├── CallTool(toolName, args)    ← 执行工具调用
  │  ├── IsConnected()               ← 检查连接状态
  │  ├── GetName()                   ← 获取服务器名称
  │  └── CreateClient()              ← 根据配置创建 MCPClient
```

### 3.3 传输类型

`MCPEndpointConfig` 支持三种传输类型：

| 类型 | 配置 | 说明 |
|------|------|------|
| `STDIO` | command + args + env | 启动子进程，通过 stdin/stdout 通信 |
| `SSE` | url | Server-Sent Events，单向推送 + HTTP POST |
| `STREAMABLE_HTTP` | url + headers | Streamable HTTP，双向 HTTP 通信 |

### 3.4 MCPEndpointConfig

```cpp
struct MCPEndpointConfig {
    std::string command;                          // STDIO 模式的启动命令
    std::vector<std::string> args;                // 命令参数
    std::string url;                              // HTTP/SSE 模式的端点 URL
    MCPTransportType transportType;               // 传输类型
    std::unordered_map<string, string> env;       // STDIO 模式的环境变量
    std::unordered_map<string, string> headers;   // HTTP 模式的自定义请求头
};
```

### 3.5 Connect 流程

```
Connect()
  │  1. CreateClient()
  │     ├── 根据 config_.transportType 创建对应的传输客户端
  │     ├── STDIO → 启动子进程 + 管道通信
  │     ├── SSE → HTTP 连接 + SSE 流
  │     └── STREAMABLE_HTTP → HTTP 连接
  │
  │  2. client_->Initialize()
  │     ├── 协议握手
  │     └── 获取 sessionId
  │
  │  3. ListTools()
  │     ├── client_->ListTools() → availableTools_
  │     └── 对每个 MCPToolInfo 创建 MCPTool 代理
  │
  │  4. connected_ = true
```

### 3.6 CallTool 流程

```
CallTool(toolName, arguments)
  │  1. lock(callMutex_)
  │  2. client_->CallTool(toolName, arguments)
  │     ├── 发送 JSON-RPC tools/call 请求
  │     ├── 接收并解析响应
  │     └── 返回 shared_ptr<MCPToolResult>
  │  3. unlock
```

### 3.7 shared_from_this

`MCPConnection` 继承 `std::enable_shared_from_this`，因为 `MCPTool` 持有 `shared_ptr<MCPConnection>`。这确保：
- 连接在最后一个工具引用释放后才销毁
- 工具代理可以安全地访问连接方法
- 避免裸指针导致的悬空引用

## 4. MCPTool 本地代理

### 4.1 设计意图

`MCPTool` 将 MCP 服务器上的远程工具封装为 `Tool` 子类，使 Agent 可以像调用本地工具一样调用 MCP 工具。

### 4.2 类定义

```cpp
class MCPTool : public Tool {
public:
    MCPTool(std::string name, std::string description,
            std::vector<ToolParam> params,
            std::shared_ptr<MCPConnection> server);
    std::string Invoke(const std::string& input) override;
private:
    std::shared_ptr<MCPConnection> server_;
};
```

### 4.3 Invoke 实现

```
MCPTool::Invoke(input)
  │  1. 解析 input → nlohmann::json arguments
  │  2. server_->CallTool(name_, arguments)
  │     ├── MCPConnection::CallTool
  │     │   ├── MCPClient::CallTool
  │     │   │   ├── SendRequest(tools/call)
  │     │   │   └── 解析响应
  │     │   └── 返回 MCPToolResult
  │  3. 格式化结果:
  │     ├── isError → "Error: {content}"
  │     └── content → 拼接所有 content 条目
  │  4. 返回文本结果
```

### 4.4 Schema 来源

MCPTool 的 Schema 来自 MCP 服务器的 `tools/list` 响应。注册时：

```
MCPConnection::Connect()
  │  ├── ListTools() → availableTools_
  │  └── 对每个 MCPToolInfo:
  │      ├── 提取 name, description, inputSchema
  │      ├── 将 inputSchema 转换为 ToolParam 列表
  │      ├── 创建 MCPTool(name, desc, params, shared_from_this())
  │      └── ResourceManager::RegisterMcpTool(name, factory)
```

## 5. MCPConfigManager 运行时配置管理

### 5.1 设计意图

`MCPConfigManager` 管理 MCP 服务器的运行时配置变更，支持增量更新（启动新服务器、停止旧服务器、重连变更的服务器）。

### 5.2 类结构

```
MCPConfigManager (Meyers 单例)
  │  ├── mutex_ (mutex)
  │  ├── lastConfigs_ (map<string, McpServerConfig>) ← 当前配置
  │  ├── servers_ (map<string, shared_ptr<MCPConnection>>) ← 活跃连接
  │  │
  │  ├── Load(configs)     ← 批量加载配置
  │  ├── Apply(config)     ← 应用单个配置（启动/重连）
  │  ├── Remove(id)        ← 移除服务器（停止连接）
  │  ├── StopAll()         ← 停止所有服务器
  │  ├── GetAllConfigs()   ← 获取所有配置
  │  ├── ActiveIds()       ← 获取活跃服务器 ID
  │  │
  │  ├── ConfigChanged()   ← 检测配置变更
  │  ├── StartServerLocked() ← 启动服务器（加锁）
  │  └── StopServerLocked() ← 停止服务器（加锁）
```

### 5.3 增量更新策略

```
Load(newConfigs)
  │  ├── 对每个新配置:
  │  │   ├── 若 id 不在 lastConfigs_ → Apply(config) → 新服务器
  │  │   ├── 若 id 在 lastConfigs_ 且 ConfigChanged → Apply(config) → 重连
  │  │   └── 若 id 在 lastConfigs_ 且未变更 → 无操作
  │  ├── 对不在新配置中的旧 id:
  │  │   └── Remove(id) → 停止并移除
  │  └── lastConfigs_ = newConfigs
```

### 5.4 ConfigChanged 判断

比较两个 `McpServerConfig` 的关键字段：

- `type`（传输类型）
- `url` / `command` / `args`（连接参数）
- `env` / `headers`（环境和请求头）
- `enabled`（启用状态）

任何字段变更都触发重连。

### 5.5 与 ResourceManager 的协作

`MCPConfigManager` 管理连接生命周期，`ResourceManager` 管理工具注册。应用层协调两者：

```
jiuwenClaw McpServerManager
  │  ├── 从配置文件加载 → McpServerConfig 列表
  │  ├── MCPConfigManager::Load(configs)
  │  │   → StartServerLocked → 创建 MCPConnection → Connect
  │  │   → 将连接存入 servers_ map
  │  ├── ResourceManager::LoadMCPServers(configs)
  │  │   → RegisterMCPServer → 注册工具到 ResourceManager
  │  └── 配置变更时:
  │      ├── MCPConfigManager::Load(newConfigs) → 增量更新
  │      └── ResourceManager 同步工具注册变更
```

## 6. 工具发现与注册流程

### 6.1 完整发现流程

```
1. 配置加载
   McpServerConfig → MCPEndpointConfig 转换

2. 连接建立
   MCPConnection::Connect()
     ├── CreateClient() → 根据传输类型创建客户端
     ├── client_->Initialize() → 协议握手
     └── client_->ListTools() → 发现工具

3. 工具注册
   对每个发现的 MCPToolInfo:
     ├── 创建 MCPTool(name, desc, params, connection)
     ├── ResourceManager::RegisterMcpTool(name, factory)
     │   ├── factory 创建 MCPTool 代理
     │   ├── toolSchemas_ 缓存 Schema
     │   └── mcpToolNames_ 标记为 MCP 工具

4. Agent 使用
   AgentConfig::mcpServerIds 列出所需的 MCP 服务器
   Agent::SyncMcpTools() 同步工具列表
   Agent::AddTools(mcpToolNames) 添加到可用工具列表

5. 调用
   ReactAgentWorker::ExecuteTool("weather", input)
     ├── ResourceManager::HasTool("weather") → true
     ├── CreateTool("weather") → MCPTool 代理
     └── MCPTool::Invoke(input) → MCPConnection::CallTool → MCPClient
```

### 6.2 动态更新

MCP 工具列表在服务器连接时一次性发现。如需更新：

- `MCPConnection::ListTools()` → 重新发现
- `ResourceManager::UnregisterMcpTool` → 移除旧工具
- `ResourceManager::RegisterMcpTool` → 注册新工具

### 6.3 MCP 工具与本地工具的区分

`mcpToolNames_` 集合跟踪所有 MCP 工具名称。这种区分允许：
- 服务器变更时批量更新 MCP 工具
- MCP 工具与本地工具分开管理
- `ResourceManager::GetMcpToolNames()` 专门查询 MCP 工具

## 7. 线程安全

| 共享状态 | 保护机制 | 说明 |
|---------|---------|------|
| MCPClient::sessionId_ | sessionMutex_ | 协议会话状态 |
| MCPClient::nextRequestId_ | atomic<int> | 请求 ID 递增 |
| MCPConnection::connected_ | stateMutex_ | 连接状态 |
| MCPConnection::availableTools_ | stateMutex_ | 工具列表 |
| MCPConnection::CallTool | callMutex_ | 工具调用串行化 |
| MCPConfigManager::lastConfigs_/servers_ | mutex_ | 配置和连接池 |
| ResourceManager 工具域 | toolMutex_ | 工具注册/注销 + schema 缓存 + MCP 工具名 |
| ResourceManager 模型域 | modelMutex_ | 模型注册/注销 |
| ResourceManager 内存域 | memoryMutex_ | MemoryRuntime 注册 |
| ResourceManager MCP 域 | mcpMutex_ | MCP 连接实例注册/查询 |

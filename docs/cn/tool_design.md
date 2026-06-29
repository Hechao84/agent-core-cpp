# 工具系统设计文档

## 1. 模块概述

工具系统（`include/tool.h`、`src/tools/`）是 Agent 框架的执行层，提供 Agent 与外部世界交互的能力。工具系统包含：

- `Tool` 抽象基类 — 所有工具的统一接口
- 12 个无状态内置工具 — 独立于会话的通用能力
- 6 个会话级工具 — 依赖会话上下文的交互能力
- `ToolSelector` — 工具选择与排序引擎
- `MCPTool` — MCP 服务器工具的本地代理

## 2. Tool 抽象基类

### 2.1 类定义

```cpp
class Tool {
public:
    Tool(std::string name, std::string description, std::vector<ToolParam> params);
    virtual ~Tool() = default;

    virtual std::string Invoke(const std::string& input) = 0;

    std::string GetName() const;
    std::string GetDescription() const;
    std::vector<ToolParam> GetParams() const;
    std::string GetSchema() const;                    // JSON Schema 字符串
    virtual nlohmann::json GetJsonSchema() const;     // 结构化 JSON Schema

protected:
    std::string name_;
    std::string description_;
    std::vector<ToolParam> params_;
};
```

### 2.2 ToolParam 结构

```cpp
struct ToolParam {
    std::string name;          // 参数名
    std::string description;   // 参数描述
    std::string type;          // 类型（"string", "number", "boolean", "array" 等）
    bool required{false};      // 是否必需
};
```

### 2.3 Schema 自动构建

`GetJsonSchema()` 默认从 `ToolParam` 列表自动构建 JSON Schema：

```json
{
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "File path to read"
    },
    "offset": {
      "type": "number",
      "description": "Line number to start reading from"
    }
  },
  "required": ["path"]
}
```

工具可覆写 `GetJsonSchema()` 以提供更复杂的 Schema（如枚举、嵌套对象、默认值等）。

### 2.4 Command 模式

每个工具本质上是一个**Command 对象**：`Invoke(input)` 执行操作并返回结果字符串。输入是 JSON 编码的参数对象，输出是文本结果。

## 3. 无状态内置工具

### 3.1 工具清单

| 名称 | 类 | 描述 | 关键参数 |
|------|-----|------|---------|
| `time_info` | `TimeInfoTool` | 获取当前日期/时间/星期 | `format`（可选） |
| `web_search` | `WebSearchTool` | 网络搜索（支持引擎回退） | `query`（必需） |
| `web_fetcher` | `WebFetcherTool` | 获取网页内容 | `url`（必需） |
| `read_file` | `ReadFileTool` | 读取文件内容 | `path`（必需）、`offset`/`limit`（可选） |
| `write_file` | `WriteFileTool` | 写入文件 | `path`（必需）、`content`（必需） |
| `edit_file` | `EditFileTool` | 原地编辑文件 | `path`、`old_string`、`new_string`（均必需） |
| `list_dir` | `ListDirTool` | 列出目录内容 | `path`（必需） |
| `glob` | `GlobTool` | 文件路径模式匹配 | `pattern`（必需）、`path`（可选） |
| `grep` | `GrepTool` | 文件内容搜索 | `pattern`（必需）、`path`（可选） |
| `exec` | `ExecTool` | 执行 Shell 命令 | `command`（必需）、`timeout`（可选） |
| `skill_search` | `SkillSearchTool` | 搜索并加载技能指令 | `query`（必需） |
| `file_state` | `FileStateTool` | 跟踪文件状态变化 | `path`（必需） |

### 3.2 文件操作工具族

`read_file`、`write_file`、`edit_file`、`list_dir`、`glob`、`grep`、`file_state` 构成文件操作工具族，赋予 Agent 完整的文件系统交互能力。

`edit_file` 采用**精确字符串替换**模式（类似 sed），而非行号模式，因为 LLM 生成的行号经常不准确。

### 3.3 Web 工具族

`web_search` 和 `web_fetcher` 提供网络访问能力。`web_search` 支持多搜索引擎回退策略。

### 3.4 SkillSearchTool 特殊设计

`SkillSearchTool` 使用**静态 `SkillEngine*` 指针**而非 `ToolBuildContext` 注入：

```cpp
// Agent 初始化时设置
static SkillEngine* skillEnginePtr_ = nullptr;

// Agent::构造
skillSearchTool.setSkillEngine(skillEngine_.get());
```

这是因为 `SkillEngine` 是 Agent 级别（而非会话级别）的资源，所有会话共享同一技能引擎。

## 4. 会话级工具

### 4.1 设计意图

会话级工具依赖会话上下文资源，每次调用时通过 `ToolBuildContext` 注入依赖。这确保同一工具在不同会话中使用不同的数据源。

### 4.2 工具清单

| 名称 | 类 | 注入依赖 | 用途 |
|------|-----|---------|------|
| `todo_create` | `TodoCreateTool` | `SessionTodoList*` | 创建任务列表 |
| `todo_complete` | `TodoCompleteTool` | `SessionTodoList*` | 标记任务完成 |
| `todo_insert` | `TodoInsertTool` | `SessionTodoList*` | 在指定位置插入任务 |
| `todo_remove` | `TodoRemoveTool` | `SessionTodoList*` | 移除任务 |
| `todo_list` | `TodoListTool` | `SessionTodoList*` | 列出当前任务 |
| `ask_user` | `AskUserTool` | `AskUserDispatcher*` + `streamCallback` | 向用户请求澄清 |
| `memory_read_payload` | `MemoryReadPayloadTool` | `MemoryRuntime*` | 读取 offloaded payload |

### 4.3 Todo 工具族

Todo 工具族通过 `SessionTodoList*` 指针操作同一个任务列表实例。每次创建工具时注入当前会话的 `SessionTodoList`：

```cpp
// 注册
rm.RegisterSessionTool("todo_create",
    [](const ToolBuildContext& ctx) -> unique_ptr<Tool> {
        return make_unique<TodoCreateTool>(ctx.todoList);
    });

// 使用
auto ctx = ToolBuildContext{.todoList = workerEnv_->GetOrCreateSessionTodoList(sid), ...};
auto tool = rm.CreateSessionTool("todo_create", ctx);
tool->Invoke(input);  // 操作会话的任务列表
```

### 4.4 AskUserTool

`AskUserTool` 是最复杂的会话级工具，实现了**异步问答**模式。

`AskUserDispatcher` 是会话级资源（随 `SessionEntry` 存活），构造时带上 `sessionId` 和 `AskUserRouter*`。`EmitAskUser` 注册 requestId→sessionId 索引（通过 `AskUserRouter`），使 `SessionManager::ProvideUserResponse(requestId, answer)` 能路由到正确会话，即使 Agent 已被热重载。

```
AskUserTool::Invoke(input)
  │  1. 解析 input → 提取问题内容
  │  2. 生成 requestId
  │  3. askUser_->EmitAskUser(requestId, payload, streamCallback)
  │     ├── 注册 Slot (条件变量 + 可选答案)
  │     ├── AskUserRouter::RegisterAskRequest(requestId, sessionId)
  │     └── 发送 [ASK_USER] 标记到流式回调
  │  4. askUser_->WaitForResponse(requestId, timeout)
  │     ├── 阻塞等待 (条件变量)
  │     ├── 完成或超时后 AskUserRouter::UnregisterAskRequest(requestId)
  │     └── 超时返回 nullopt → 工具返回超时消息
  │  5. 收到 answer → 返回答案字符串
```

应用层回应路由：
```
POST /api/answer → SessionManager::ProvideUserResponse(requestId, answer)
  │  1. 查 askRequestToSession_ 索引 → 得到 sessionId
  │  2. 查 sessions_ → 得到 SessionEntry->askUser
  │  3. SessionEntry->askUser->ProvideResponse(requestId, answer)
```

### 4.5 MemoryReadPayloadTool

配合 Payload Offloading 机制使用：

```
MemoryReadPayloadTool::Invoke(input)
  │  1. 解析 input → 提取 payloadRef (URI)
  │  2. memoryRuntime_->ReadPayload(uri)
  │  3. 返回完整 payload 内容
```

## 5. ToolSelector 工具选择器

### 5.1 设计意图

`ToolSelector` 为工具选择提供排序和筛选能力。当前实现为 stub（所有工具返回均匀分数 1.0），预留了关键词匹配和 embedding 相似度的扩展接口。

### 5.2 类结构

```cpp
class ToolSelector {
public:
    ToolSelector(SearchConfig config = {});
    void AddToolToPool(const std::string& toolName);
    void RemoveToolFromPool(const std::string& toolName);
    std::string SelectTool(const std::string& query, const std::vector<string>& available);
    std::vector<string> SelectTopTools(const std::string& query, const std::vector<string>& available, int maxCount);
    std::vector<ToolMatchResult> RankTools(const std::string& query, const std::vector<string>& available);
private:
    SearchConfig config_;
    std::vector<string> toolPool_;
    selectionStrategy_ (function);
    vector<ToolMatchResult> ScoreTools(query, available);
    double CalculateKeywordScore(query, toolName, toolDesc);
    double CalculateEmbeddingScore(query, toolName);
};
```

### 5.3 SearchConfig

```cpp
struct SearchConfig {
    double keywordWeight{0.5};
    double embeddingWeight{0.5};
    string embeddingModelName;
};
```

### 5.4 ToolMatchResult

```cpp
struct ToolMatchResult {
    string toolName;
    double score;
    string reason;
};
```

### 5.5 未来扩展

当工具数量超过模型原生 function calling 的限制时，`ToolSelector` 可用于：
- 根据查询筛选最相关的 N 个工具
- 减少发送给模型的工具 Schema 数量
- 提高模型选择正确工具的准确率

## 6. MCPTool 代理模式

### 6.1 设计意图

`MCPTool` 是 MCP 服务器工具的本地代理，将远程工具封装为 `Tool` 子类，使 Agent 可以像调用本地工具一样调用 MCP 工具。

### 6.2 类定义

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

### 6.3 Invoke 流程

```
MCPTool::Invoke(input)
  │  1. 解析 input → nlohmann::json arguments
  │  2. server_->CallTool(name_, arguments)
  │     ├── MCPConnection 路由到 MCPClient
  │     ├── MCPClient 发送 JSON-RPC tools/call 请求
  │     └── 接收并解析结果
  │  3. 将 MCPToolResult 格式化为文本
  │     ├── isError → 返回错误消息
  │     └── content → 返回拼接的内容文本
```

### 6.4 Schema 来源

MCPTool 的 Schema 来自 MCP 服务器的 `tools/list` 响应：

```json
// MCP tools/list 响应
{"tools": [{"name": "weather", "description": "Get weather", "inputSchema": {"type": "object", "properties": {"city": {"type": "string"}}}}]}
```

注册时将 `inputSchema` 转换为 `ToolParam` 列表（简化映射），或直接覆写 `GetJsonSchema()` 返回原始 Schema。

## 7. 工具注册与发现流程

### 7.1 启动时注册

```
ResourceManager 构造
  │  ├── RegisterBuiltinTools()
  │  │   ├── RegisterTool("time_info", TimeInfoTool factory)
  │  │   ├── RegisterTool("read_file", ReadFileTool factory)
  │  │   ├── ... 12 个无状态工具
  │  │   ├── RegisterSessionTool("todo_create", TodoCreateTool factory)
  │  │   ├── ... 6 个会话级工具
  │  │   └── 缓存 Schema
  │  │
  │  └── RegisterBuiltinModels()
```

### 7.2 应用层扩展

jiuwenClaw 在 `main.cpp` 中注册应用级工具：

```cpp
// 注册 jiuwenClaw 特有的工具
rm.RegisterTool("notebook_edit", NotebookEditTool factory);
rm.RegisterSessionTool("cron", CronTool factory);
rm.RegisterSessionTool("notify", NotifyTool factory);
```

### 7.3 MCP 工具发现

```
LoadMCPServers(configs)
  │  ├── 对每个 McpServerConfig:
  │  │   ├── RegisterMCPServer(config)
  │  │   │   ├── MCPConnection::Connect()
  │  │   │   ├── MCPConnection::ListTools() → vector<MCPToolInfo>
  │  │   │   └── 对每个 MCPToolInfo:
  │  │   │       ├── RegisterMcpTool(toolName, factory)
  │  │   │       └── factory 创建 MCPTool(name, desc, params, connection)
```

### 7.4 Agent 工具配置

`AgentConfig::defaultTools` 指定 Agent 可用的工具列表：

```cpp
config.defaultTools = rm.GetAvailableTools();  // 获取所有可用工具名称
// 或精选
config.defaultTools = {"read_file", "write_file", "exec", "web_search"};
```

`Agent::AddTools()` 和 `AgentWorker::AddTools()` 可在运行时动态添加工具。

## 8. 线程安全考虑

- **无状态工具**：每次 `CreateTool` 创建新实例，无并发问题
- **会话级工具**：每次 `CreateSessionTool` 创建新实例，注入当前会话的依赖指针
- **`SessionTodoList*`**：由 `SessionEntry` 拥有（随会话存活、跨热重载保留），通过 WorkerEnv 预缓存访问，使用时由 `invokeMutex` 保证串行
- **`AskUserDispatcher*`**：由 `SessionEntry` 拥有（随会话存活、跨热重载保留），内部有 `slotsMu_`（L6）保护
- **`MemoryRuntime*`**：由 `SessionManager` 拥有，各方法内部有锁保护
- **MCPTool**：持有 `shared_ptr<MCPConnection>`，连接内部有 `callMutex_` 保护并发调用

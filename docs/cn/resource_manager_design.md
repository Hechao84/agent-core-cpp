# 资源管理模块设计文档

## 1. 模块概述

资源管理模块（`include/resource_manager.h`、`src/resource_manager/resource_manager.cpp`）是框架的可插拔组件注册中心，采用 Meyers 单例模式，负责管理所有工具、模型、记忆运行时和 MCP 服务器的注册与创建。它是框架**插件式扩展**理念的核心实现。

## 2. ResourceManager 类设计

### 2.1 单例模式

```cpp
static ResourceManager& GetInstance();
```

使用 Meyers 单例（静态局部变量），线程安全初始化。构造时自动注册所有内置工具和模型。

### 2.2 注册表结构

```
ResourceManager
 ├── toolMutex_ (工具域：toolFactories_ + sessionToolFactories_ + schema 缓存 + mcpToolNames_)
 ├── modelMutex_ (模型域：modelFactories_ + providerModelFactories_)
 ├── memoryMutex_ (内存域：memoryFactories_)
 ├── mcpMutex_ (MCP 域：mcpServers_)
 │
 ├── 工具注册表 (两层)
 │   ├── toolFactories_ (map<string, function<unique_ptr<Tool>()>>)
 │   │   ← 无状态工具工厂
 │   ├── sessionToolFactories_ (map<string, SessionToolFactory>)
 │   │   ← 会话级工具工厂 (function<unique_ptr<Tool>(ToolBuildContext)>)
 │   ├── toolSchemas_ (map<string, string>) ← 无状态工具 JSON Schema 缓存
 │   ├── sessionToolSchemas_ (map<string, string>) ← 会话级工具 Schema 缓存
 │   ├── toolSchemaCache_ (map<string, ToolSchema>) ← 结构化 Schema 缓存
 │   └── mcpToolNames_ (unordered_set<string>) ← MCP 工具名称集合
 │
 ├── 模型注册表 (两层)
 │   ├── modelFactories_ (map<ModelFormatType, function<unique_ptr<Model>(ModelConfig)>>)
 │   │   ← 按 formatType 的标准实现
 │   ├── providerModelFactories_ (map<string, function<unique_ptr<Model>(ModelConfig)>>)
 │   │   ← 按 provider 的自定义实现
 │
 ├── 记忆运行时注册表
 │   ├── memoryFactories_ (map<string, function<unique_ptr<MemoryRuntime>(MemoryConfig)>>)
 │   │   ← 按 provider 的记忆运行时工厂
 │
 ├── MCP 服务器注册表
 │   ├── mcpServers_ (map<string, shared_ptr<MCPConnection>>)
 │   │   ← 活跃 MCP 服务器连接池
```

## 3. 两层工具注册

### 3.1 设计动机

工具分为两类：

- **无状态工具**：每次调用独立，无需会话上下文（如 `time_info`、`read_file`）
- **会话级工具**：依赖会话级资源（如 `todo_create` 需要 `SessionTodoList`，`ask_user` 需要 `AskUserDispatcher`）

两层注册确保会话级工具在创建时能接收正确的依赖注入。

### 3.2 无状态工具注册

```cpp
void RegisterTool(const std::string& name, std::function<std::unique_ptr<Tool>()> factory);
```

工厂函数无参数，每次调用创建一个全新的工具实例。注册时同时缓存其 JSON Schema：

```
RegisterTool("read_file", []() -> unique_ptr<Tool> {
    return make_unique<ReadFileTool>();
})
  │  ├── toolFactories_["read_file"] = factory
  │  ├── toolSchemaCache_["read_file"] = 构建并缓存 ToolSchema
  │  └── toolSchemas_["read_file"] = Schema JSON 字符串
```

### 3.3 会话级工具注册

```cpp
using SessionToolFactory = std::function<std::unique_ptr<Tool>(const ToolBuildContext&)>;
void RegisterSessionTool(const std::string& name, SessionToolFactory factory);
```

工厂函数接收 `ToolBuildContext`，包含会话级依赖：

```cpp
struct ToolBuildContext {
    SessionTodoList* todoList;           // 会话任务列表
    AskUserDispatcher* askUser;          // 异步问答调度器
    MemoryRuntime* memoryRuntime;        // 记忆运行时
    std::function<void(const std::string&)> streamCallback;  // 流式回调
    std::string sessionId;               // 会话 ID
};
```

### 3.4 创建流程

```
CreateTool("read_file")
   │  ├── lock(toolMutex_)
   │  ├── 查找 toolFactories_["read_file"] → 拷贝 factory 到局部变量
   │  ├── unlock(toolMutex_)
   │  ├── factory() → 锁外创建新 ReadFileTool
   │  └── 返回 unique_ptr<Tool>

CreateSessionTool("todo_create", ctx)
   │  ├── lock(toolMutex_)
   │  ├── 查找 sessionToolFactories_["todo_create"] → 拷贝 factory 到局部变量
   │  ├── unlock(toolMutex_)
   │  ├── factory(ctx) → 锁外创建 TodoCreateTool(ctx.todoList)
   │  └── 返回 unique_ptr<Tool>
```

### 3.5 Schema 构建

`BuildToolSchemas(toolNames, ctx)` 为指定工具列表构建结构化 Schema。每个工具的流程合并了 cache 查找 + 类型判定（一次锁获取）+ 锁外 probe 创建 + cache 写入（一次锁获取）：

```
BuildToolSchemas(["read_file", "todo_create"], ctx)
   │  ├── 对每个工具名:
   │  │   ├── lock(toolMutex_) → 查 cache + 判定类型（session/stateless/未知） → unlock
   │  │   ├── 若有缓存 → 直接返回
   │  │   ├── 锁外创建 probe（拷贝的 factory 在锁外调用）
   │  │   ├── lock(toolMutex_) → 写入 toolSchemaCache_ → unlock
   │  │   └── 返回 ToolSchema
   │  └── 返回 vector<ToolSchema>
```

注意：Schema 构建时创建临时工具实例仅用于获取 Schema，然后立即释放。会话级工具的 Schema 与具体 `ToolBuildContext` 无关，只需要一个探测实例。

## 4. ToolBuildContext 依赖注入

### 4.1 设计意图

`ToolBuildContext` 是会话级工具的**依赖注入桥**，将 `SessionEntry` 拥有的会话级资源通过 `WorkerEnv` → `SmWorkerEnv` → SessionManager 传递到工具实例。依赖链为 `AgentWorker → WorkerEnv(SmWorkerEnv) → SessionManager → SessionEntry`，`WorkerEnv` 不反向引用 Agent。

### 4.2 填充流程

```
ReactAgentWorker::ExecuteTool(toolName, input, streamCallback)
  │
  │  判断工具类型:
  │  ├── ResourceManager::HasSessionTool(toolName)
  │  │   ├── 构建 ToolBuildContext:
  │  │   │   ├── todoList = workerEnv_->GetOrCreateSessionTodoList(sessionId)
  │  │   │   ├── askUser = workerEnv_->GetAskUserDispatcher(sessionId)
  │  │   │   ├── memoryRuntime = workerEnv_->GetMemoryRuntime()
  │  │   │   ├── streamCallback = 当前的流式回调
  │  │   │   └── sessionId = 当前会话 ID
  │  │   ├── ResourceManager::CreateSessionTool(toolName, ctx)
  │  │   └── tool->Invoke(input)
  │  │
  │  └── ResourceManager::HasTool(toolName) (无状态)
  │      ├── ResourceManager::CreateTool(toolName)
  │      └── tool->Invoke(input)
```

### 4.3 生命周期

- `ToolBuildContext` 是临时对象，每次工具调用时构建
- 工具实例也是临时的（创建 → Invoke → 释放），不缓存
- 指针成员（`todoList*`、`askUser*`、`memoryRuntime*`）指向 `Agent` 拥有的长期对象

## 5. 模型注册与双路由

### 5.1 注册方式

**按 formatType 注册（标准实现）**：

```cpp
void RegisterModel(ModelFormatType type, function<unique_ptr<Model>(ModelConfig)> factory);
```

内置注册：
- `ModelFormatType::OPENAI` → `OpenAIModel`
- `ModelFormatType::ANTHROPIC` → `AnthropicModel`

**按 provider 注册（自定义实现）**：

```cpp
void RegisterModel(const std::string& provider, function<unique_ptr<Model>(ModelConfig)> factory);
```

### 5.2 创建路由

```
CreateModel(config)
   │  ├── lock(modelMutex_) → 查找 factory → 拷贝到局部变量 → unlock
   │  ├── 若 config.provider 非空:
   │  │   ├── 查找 providerModelFactories_[config.provider]
   │  │   ├── 若找到 → 拷贝 factory
   │  │   └── 若未找到 → 回退到 formatType 工厂
   │  │
   │  └── 若 config.provider 为空:
   │  │   ├── 查找 modelFactories_[config.formatType]
   │  │   ├── 若找到 → 拷贝 factory
   │  │   └── 若未找到 → 报错
   │  └── 锁外调用 factory(config) → 返回 Model 实例
```

这种双路由设计允许用户在同一个 formatType 下注册不同的 provider 实现，处理各厂商的协议差异。

## 6. 记忆运行时注册

### 6.1 静态注册

```cpp
void RegisterMemoryRuntime(const std::string& provider,
                           function<unique_ptr<MemoryRuntime>(MemoryConfig)> factory);
```

内置注册：
- `"builtin.compat"` → `BuiltinMemoryRuntime`（仅 `JIUWEN_ENABLE_MEMORY_BUILTIN` 编译时可用）
- `"http.server"` → `HttpMemoryRuntime`

### 6.2 动态插件加载

```cpp
void LoadMemoryPlugins(const std::string& pluginDir);
```

扫描指定目录下的 `.so`（Linux）或 `.dll`（Windows）文件，使用 `dlopen`/`LoadLibrary` 加载，调用导出的 `RegisterMemoryPlugin(ResourceManager&)` 入口函数。

插件使用 `AGENT_PLUGIN_API` 导出宏定义入口：

```cpp
// 插件实现示例
extern "C" AGENT_PLUGIN_API void RegisterMemoryPlugin(ResourceManager& rm) {
    rm.RegisterMemoryRuntime("my-memory", [](const MemoryConfig& cfg) {
        return make_unique<MyMemoryRuntime>(cfg);
    });
}
```

### 6.3 容错机制

- 目录不存在 → 忽略，不报错
- 加载失败（`dlopen`/`LoadLibrary` 错误） → 日志记录，跳过
- 符号查找失败 → 日志记录，跳过
- 一个坏插件不会中断整个启动流程

## 7. MCP 服务器生命周期管理

### 7.1 注册与连接

```cpp
void LoadMCPServers(const std::vector<McpServerConfig>& configs);
void RegisterMCPServer(const McpServerConfig& config);
```

注册流程：

```
RegisterMCPServer(config)
  │  ├── 构建 MCPEndpointConfig (从 McpServerConfig 转换)
  │  ├── 创建 MCPConnection(name, endpointConfig)
  │  ├── connection->Connect() → 初始化 + 发现工具
  │  ├── 存入 mcpServers_[name] = shared_ptr<MCPConnection>
  │  ├── 对每个发现的 MCP 工具:
  │  │   ├── RegisterMcpTool(toolName, factory) → 注册到工具表
  │  │   └── mcpToolNames_.insert(toolName) → 标记为 MCP 工具
```

### 7.2 注销与断连

```cpp
void UnregisterMCPServer(const std::string& id);
void RemoveMCPServerRecord(const std::string& id);
```

注销流程：

```
UnregisterMCPServer(id)
  │  ├── 查找 mcpServers_[id]
  │  ├── 对该 server 的所有工具:
  │  │   ├── UnregisterMcpTool(toolName) → 从工具表移除
  │  │   └── mcpToolNames_.erase(toolName)
  │  ├── connection->Disconnect()
  │  └── mcpServers_.erase(id)
```

### 7.3 与 MCPConfigManager 的协作

`MCPConfigManager` 是 MCP 配置的管理单例，负责：
- 接收 `McpServerConfig` 列表
- 检测配置变更（`ConfigChanged`）
- 启动/停止 MCP 服务器
- 将变更同步到 `ResourceManager`

应用层（jiuwenClaw 的 `McpServerManager`）协调两者：
- 从配置文件加载 → `MCPConfigManager::Load`
- 配置变更时 → 增量注册/注销 `ResourceManager` 中的服务器

## 8. 内置注册项

### 8.1 RegisterBuiltinTools

构造时自动注册 12 个无状态工具 + 6 个会话级工具：

**无状态工具**：

| 名称 | 类 | 用途 |
|------|-----|------|
| `time_info` | `TimeInfoTool` | 获取当前时间/日期 |
| `web_search` | `WebSearchTool` | 网络搜索 |
| `web_fetcher` | `WebFetcherTool` | 获取网页内容 |
| `read_file` | `ReadFileTool` | 读取文件内容 |
| `write_file` | `WriteFileTool` | 写入文件 |
| `edit_file` | `EditFileTool` | 原地编辑文件 |
| `list_dir` | `ListDirTool` | 列出目录内容 |
| `glob` | `GlobTool` | 文件路径模式匹配 |
| `grep` | `GrepTool` | 文件内容搜索 |
| `exec` | `ExecTool` | 执行 Shell 命令 |
| `skill_search` | `SkillSearchTool` | 搜索并加载技能 |
| `file_state` | `FileStateTool` | 跟踪文件状态变化 |

**会话级工具**：

| 名称 | 类 | 依赖 |
|------|-----|------|
| `todo_create` | `TodoCreateTool` | `SessionTodoList*` |
| `todo_complete` | `TodoCompleteTool` | `SessionTodoList*` |
| `todo_insert` | `TodoInsertTool` | `SessionTodoList*` |
| `todo_remove` | `TodoRemoveTool` | `SessionTodoList*` |
| `todo_list` | `TodoListTool` | `SessionTodoList*` |
| `ask_user` | `AskUserTool` | `AskUserDispatcher*` + `streamCallback` |
| `memory_read_payload` | `MemoryReadPayloadTool` | `MemoryRuntime*` |

### 8.2 RegisterBuiltinModels

构造时自动注册两个标准模型：

| formatType | 类 |
|------------|-----|
| `ModelFormatType::OPENAI` | `OpenAIModel` |
| `ModelFormatType::ANTHROPIC` | `AnthropicModel` |

## 9. 线程安全

四把域级锁（`toolMutex_`、`modelMutex_`、`memoryMutex_`、`mcpMutex_`）独立保护各自注册表，tool 注册不阻塞 model 创建。创建操作（`CreateTool`、`CreateSessionTool`、`CreateModel`、`CreateMemoryRuntime`）在锁内查找 factory 并拷贝到局部变量，锁外调用 factory 创建实例，确保构造（可能较慢）不阻塞注册表。

`GetAvailableTools()` 等查询方法获取对应域的锁，遍历注册表期间阻塞同域的注册/注销但不影响其他域。

MCP 工具名集合（`mcpToolNames_`）属于 tool 域（`toolMutex_`），MCP 连接实例（`mcpServers_`）属于 MCP 域（`mcpMutex_`）。两者独立，不互相阻塞。

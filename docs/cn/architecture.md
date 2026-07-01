# jiuwen-lite 架构文档

## 1. 项目定位与核心理念

jiuwen-lite 是一个**轻量级、模块化的 C++ AI Agent 框架**，用于构建具备工具调用能力的推理型智能体。其设计遵循以下核心理念：

- **核心库与传输层解耦**：框架核心以共享库（`agent_framework`）形式提供，不包含 HTTP、Web UI、IM 机器人等传输层代码，这些由参考应用 `jiuwenClaw` 在 `examples/` 下独立实现。
- **插件式扩展**：模型适配器、工具、记忆运行时均可通过注册表动态注册，支持 `.so`/`.dll` 插件加载。
- **会话级隔离**：每个会话拥有独立的 `ContextEngine`、任务列表和流式回调，通过 `SessionManager` 进行统一管理和并发控制。
- **热重载安全**：支持在不中断运行的情况下原子替换 Agent 配置，保留会话上下文和记忆运行时。

所有代码位于 `namespace jiuwen` 下，公开 API 在 `include/` 头文件中声明，通过 `AGENT_API` 宏导出符号。

## 2. 架构分层

```
┌─────────────────────────────────────────────────────────────┐
│                      应用层 (Application)                    │
│  jiuwenClaw: CLI / HTTP REST + SSE / 飞书 / 心跳 / 定时任务 │
├─────────────────────────────────────────────────────────────┤
│                      传输适配层 (Adapters)                   │
│  HTTP Server Adapter │ Feishu Bot Adapter │ Channel Service │
├─────────────────────────────────────────────────────────────┤
│                    框架核心库 (agent_framework)               │
│                                                              │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌───────────────┐  │
│  │ Session  │ │ Resource │ │  Agent   │ │  AgentWorker  │  │
│  │ Manager  │ │ Manager  │ │  (Facade)│ │ (ReAct Loop) │  │
│  └──────────┘ └──────────┘ └──────────┘ └───────────────┘  │
│                                                              │
│  ┌──────┐ ┌──────────┐ ┌──────┐ ┌───────┐ ┌────────────┐  │
│  │Model │ │Context   │ │Memory│ │  MCP  │ │   Skills   │  │
│  │Adapter│ │Engine   │ │Runtime│ │Client │ │   Engine   │  │
│  └──────┘ └──────────┘ └──────┘ └───────┘ └────────────┘  │
│                                                              │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              工具系统 (Tool System)                    │   │
│  │  12 无状态工具 │ 6 会话级工具 │ MCP 工具 │ 自定义工具 │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                              │
│  ┌───────────────────┐ ┌──────────────┐                    │
│  │  配置系统 (Config) │ │  工具类(Util) │                    │
│  └───────────────────┘ └──────────────┘                    │
├─────────────────────────────────────────────────────────────┤
│                   第三方依赖 (Third Party)                    │
│  nlohmann/json │ SQLite3 │ libcurl │ cpp-httplib │ OpenSSL │
│  agent-memory-cpp (可选)                                     │
└─────────────────────────────────────────────────────────────┘
```

## 3. 模块全景与依赖关系

```
AgentConfig ───────────────────────────────────────────────┐
  │  ├── ModelConfig                                       │
  │  ├── ContextConfig                                     │
  │  ├── DreamConfig                                       │
  │  ├── MemoryConfig                                      │
  │  └── McpServerConfig                                   │
  │                                                         │
SessionManager ─── 拥有 Agent (shared_ptr)                  │
  │  ├── 拥有 MemoryRuntime (unique_ptr, 跨热重载存活)       │
  │  ├── 管理 SessionEntry ─── 拥有 ContextEngine           │
  │  └── 全局并发门控 + 热重载屏障                           │
  │                                                         │
Agent ─── 拥有 AgentWorker                                  │
  │  ├── 拥有 SkillEngine                                   │
  │  ├── 拥有 HistoryStore                                  │
  │  ├── 拁有 LongTermConsolidator                          │
  │  ├── 非拥有 SessionTodoList* (per-session, 由 SessionEntry 拥有, 经 WorkerEnv 访问) │
  │  ├── 非拥有 AskUserDispatcher* (per-session, 由 SessionEntry 拥有, 经 WorkerEnv 访问) │
  │  ├── 非拥有 MemoryRuntime* (来自 SessionManager)        │
  │  └── ConsolidationLoop 后台线程                         │
  │                                                         │
AgentWorker (WorkerEnv 接口隔离)                             │
  │  ├── 使用 ContextEngine (由 SessionManager 注入)        │
  │  ├── 使用 ResourceManager (创建工具/模型)               │
  │  ├── 使用 ToolSelector                                  │
  │  └── 使用 SkillEngine                                   │
  │                                                         │
ResourceManager ─── Meyers 单例注册表                        │
  │  ├── 工具工厂 (无状态 + 会话级)                          │
  │  ├── 模型工厂 (按 formatType + provider)                │
  │  ├── 记忆运行时工厂                                     │
  │  ├── MCP 服务器连接池                                   │
  │  └── 动态插件加载器                                     │
  │                                                         │
ContextEngine ─── 管理会话上下文窗口                         │
  │  ├── 存储后端 (MEMORY_ONLY / JSON_FILE / DATABASE)      │
  │  ├── memoryContextProvider_ ──→ MemoryRuntime::BuildContext│
  │  ├── memoryEventSink_ ──→ MemoryRuntime::AppendEvent    │
  │  └── 消息分段 + 压缩算法                                │
  │                                                         │
MemoryRuntime ─── 可插拔记忆接口                             │
  ├── BuiltinMemoryRuntime ─── 桥接 agent-memory-cpp        │
  ├── HttpMemoryRuntime ─── HTTP 远程代理                    │
  └── 动态插件 (.so/.dll)                                   │
  │                                                         │
MCP ─── Model Context Protocol 集成                         │
  ├── MCPClient ─── JSON-RPC 协议                           │
  ├── MCPConnection ─── STDIO / SSE / Streamable HTTP       │
  ├── MCPTool ─── 本地代理                                  │
  └── MCPConfigManager ─── 运行时配置管理                    │
```

## 4. 核心执行流程

### 4.1 一次完整的 Agent Invoke

```
用户输入
  │
  ▼
SessionManager::Invoke(sessionId, message, callback)
  │  1. 入口内联门控: 等待 !reloading_ + 并发槽位
  │  2. 查找/创建 SessionEntry → ContextEngine
  │  3. 获取 Agent (shared_ptr，热重载安全)
  │
  ▼
Agent::Invoke(sessionId, query, callback)
  │  1. 添加用户消息到 ContextEngine
  │  2. 调用 AgentWorker::Invoke()
  │
  ▼
ReactAgentWorker::Invoke(query, contextEngine, callback)
  │  1. CurrentCancelGeneration() → 获取 generation counter
  │  2. 进入 ReactLoop()
  │
  ▼
ReactLoop —— 循环执行直到 isFinished 或 maxIterations
  │
  │  ┌───────────────────────────────────────────────────┐
  │  │ 迭代步骤:                                         │
  │  │                                                   │
  │  │  1. 组装系统提示                                   │
  │  │     - promptTemplates + 技能目录 + 记忆上下文       │
  │  │     - {$memory} → MemoryRuntime::BuildContext()   │
  │  │     - todo 列表片段                               │
  │  │                                                   │
  │  │  2. 获取上下文窗口                                │
  │  │     - ContextEngine::GetContextWindow()           │
  │  │     - ApplyContextLimits → 消息分段 + 压缩         │
  │  │                                                   │
  │  │  3. 构建工具 Schema                               │
  │  │     - ResourceManager::BuildToolSchemas()         │
  │  │                                                   │
  │  │  4. 调用模型                                      │
  │  │     - Model::Format() → 生成请求体                 │
  │  │     - Model::Invoke() → 流式返回                   │
  │  │     - 解析 tool_calls 或提示词回退                  │
  │  │                                                   │
  │  │  5. 判断响应类型                                  │
  │  │     - 有 tool_calls → 执行工具                     │
  │  │     - 无 tool_calls → 作为最终回答返回              │
  │  │                                                   │
  │  │  6. 执行工具                                      │
  │  │     - ResourceManager::CreateTool / CreateSessionTool│
  │  │     - Tool::Invoke(input)                          │
  │  │     - Payload Offloading (可选)                    │
  │  │                                                   │
  │  │  7. 追加观察结果到 ContextEngine                    │
  │  │     - assistant tool_calls 消息                    │
  │  │     - tool result 消息                             │
  │  │                                                   │
  │  │  8. 继续循环                                      │
  │  └───────────────────────────────────────────────────┘
  │
  ▼
返回最终回答字符串
```

### 4.2 记忆整合流程

```
空闲会话触发 (idleConsolidationSeconds)
  │
  ▼
Agent::ConsolidationLoop (后台线程)
  │  条件变量等待，直到有会话空闲
  │
  ▼
MemoryRuntime::Consolidate(request, modelClient)
  │  └── HostMemoryModelClient 适配框架 Model → MemoryModelClient
  │
  ▼
(当 MemoryRuntime 未启用时的遗留路径)
DreamProcessor::Run(model, historyStore)
  │  Phase 1: 分析近期历史 → 提取事实 (DreamFinding)
  │  Phase 2: 执行文件编辑操作 → 更新 MEMORY.md / SOUL.md / USER.md
```

## 5. 线程模型与并发控制

### 5.1 线程构成

| 线程 | 来源 | 作用 |
|------|------|------|
| 主线程 | 应用层 | HTTP server / CLI 事件循环 |
| Invoke 线程 | SessionManager | 每次调用在应用层线程上执行（非框架创建） |
| ConsolidationLoop | Agent::构造 | 后台记忆整合 |
| ConfigWatcher | 应用层可选 | 配置文件轮询 |

### 5.2 并发控制机制

框架通过三层并发保护确保线程安全：

```
全局并发门控 (SessionManager)
  │  maxConcurrentSessions 限制同时执行的 Invoke 数量
  │  Invoke 入口内联门控 + releaseGate 释放（无独立 Acquire/Release 方法）
  │  热重载屏障: reloading_ flag + reloadCv_
  │
  ▼
每会话串行锁 (SessionEntry::invokeMutex)
  │  同一会话的调用串行执行
  │  防止同一 ContextEngine 的并发读写
  │
  ▼
对象级互斥 (各模块内部 mutex)
   │  ResourceManager::toolMutex_ — 工具注册表 + schema 缓存 + MCP 工具名
   │  ResourceManager::modelMutex_ — 模型注册表
   │  ResourceManager::memoryMutex_ — MemoryRuntime 注册表
   │  ResourceManager::mcpMutex_ — MCP 连接实例
   │  ContextEngine::memoryMutex_ — 内存缓冲区
   │  AskUserDispatcher::slotsMu_ — 答复槽位
   │  MCPConnection::stateMutex_ / callMutex_ — 连接状态
```

### 5.3 锁排序协议

系统中所有 mutex 按层级编号（Lock layer），**低层号锁必须先获取、后释放；高层号锁在低层号锁仍被持有时才可获取**。禁止反向获取（高层号先、低层号后），以防止死锁。

| 层号 | Mutex | 所属对象 | 保护范围 |
|------|-------|----------|----------|
| L0 | `g_initMutex` | anonymous namespace | SessionManager 单例初始化（启动时单线程） |
| L1 | `concurrencyMutex_` | SessionManager | 全局并发门控 + reload barrier |
| L2 | `sessionMutex_` | SessionManager | 会话注册表 (`sessions_`) |
| L3 | `invokeMutex` | SessionEntry | 每会话调用串行化 |
| L3 | `consolidationMutex_` | Agent | 整合线程条件变量（与 invokeMutex 独立同层） |
| L4 | `sessionActivityMutex_` | Agent | 会话忙闲追踪 (`sessionActivity_`) |
| L4 | `askIndexMutex_` | SessionManager | ask_user 请求路由 (`askRequestToSession_`) |
| L5 | `ResourceManager::toolMutex_` | ResourceManager | 工具注册表 + schema 缓存 |
| L5 | `ResourceManager::modelMutex_` | ResourceManager | 模型注册表 |
| L5 | `ResourceManager::memoryMutex_` | ResourceManager | 记忆运行时工厂 + 插件 handle |
| L5 | `ResourceManager::mcpMutex_` | ResourceManager | MCP 连接实例 |
| L5 | `AgentWorker::toolMutex_` | AgentWorker | 工具名列表 + 选择器 |
| L5 | `ConfigWatcher::mutex_` | ConfigWatcher | watch 条目（不在 Invoke 线程） |
| L5 | `AgentConfigStore::mutex_` | AgentConfigStore | 配置持久化（不在 Invoke 线程） |
| L5 | `MCPConfigManager::mutex_` | MCPConfigManager | MCP 配置（不在 Invoke 线程） |
| L6 | `ContextEngine::memoryMutex_` | ContextEngine | 消息缓冲区 + 存储后端 |
| L6 | `AskUserDispatcher::slotsMu_` | AskUserDispatcher | pending ask_user slots |
| L6 | `MCPConnection::stateMutex_` | MCPConnection | 连接状态 + 自动重连 |
| L6 | `SessionTodoList::mu_` | SessionTodoList | todo 列表操作 |
| L7 | `MCPConnection::callMutex_` | MCPConnection | 工具调用串行化 |
| L7 | `MCPClient::sessionMutex_` | MCPClient | JSON-RPC session ID |
| L8 | `AskUserDispatcher::Slot::m` | Slot | 每请求条件变量等待 |

**关键约束**：

1. **L2 `sessionMutex_` 总是释放后才获取 L4+ 锁**——绝不与 L4 及以上锁同时持有。所有需同时访问 `sessions_` 和 `sessionActivity_` 的方法（如 `IsSessionBusy`、`CleanupSession`、`RemoveSession`）均在释放 `sessionMutex_` 后才获取 `sessionActivityMutex_`。

2. **L5 ResourceManager 四把域锁互不嵌套**——同一线程绝不会同时持两把域锁。它们保护不同的注册表，各域独立并发。

3. **L5 配置锁（ConfigWatcher/AgentConfigStore/MCPConfigManager）不在 Invoke 线程上获取**，与 Invoke 路径的锁无交叉嵌套风险。但 `MCPConfigManager::mutex_` 在配置管理线程上嵌套 L5 `mcpMutex_` 和 L6 `stateMutex_`。

4. **Invoke 期间 SmWorkerEnv 预缓存 SessionEntry**——在 Invoke 入口处，SessionManager 通过 `WorkerEnv::SetCurrentEntry` 缓存 `shared_ptr<SessionEntry>`，SmWorkerEnv 的 `GetContextEngine`/`GetOrCreateSessionTodoList`/`GetAskUserDispatcher` 直接从缓存 entry 返回资源，不再获取 `sessionMutex_`，消除了 invokeMutex→sessionMutex_ 嵌套。Invoke 完成后调用 `ClearCurrentEntry` 清除缓存。

5. **L0 `g_initMutex` 仅在启动时单线程持有**——`InitSessionManager` 中可嵌套 `sessionMutex_`、`ResourceManager::memoryMutex_`、`AgentWorker::toolMutex_` 等，但此时无并发访问，不存在死锁风险。

**层号标注**：每个 mutex 声明旁均有 `// Lock layer L<n>` 注释，开发者在头文件中即可看到排序约束。

### 5.4 取消机制

`AgentWorker::Cancel()` 通过递增 `cancelGeneration_` 实现：

- 每次 `CurrentCancelGeneration()` 返回当前 generation 值（不递增），作为本次 invocation 的基线快照
- `IsCancelled(myGeneration)` 检查 `cancelGeneration_ > myGeneration`（已取消返回 true）
- 取消时递增 100，确保所有正在运行的 invocation 都失效
- 不同会话共享同一 worker，但各自持有不同的 generation，因此一次取消不会影响其他会话

## 6. 热重载机制

```
SessionManager::ReloadAgent(newConfig)
  │
  │  1. 设置 reloading_ = true
  │     → 新的 Invoke 调用被阻塞等待
  │
  │  2. 等待并发门控归零 (所有进行中的 Invoke 完成)
  │     → concurrentCount_ == 0 + concurrencyCv_
  │
  │  3. 构建新 Agent
  │     → Agent(newConfig)
  │     → SetupAgentContextRouting()
  │     → SetMemoryRuntime(memoryRuntime_.get())
  │
  │  4. 原子替换
  │     → agent_ = make_shared<Agent>(newAgent)
  │     → 旧 Agent 调用 Cancel()
  │
  │  5. 清除 reloading_ 标志
  │     → 释放阻塞的 Invoke 调用
  │
  │  关键保障:
  │  - GetAgent() 返回 shared_ptr，调用者持有强引用
  │  - MemoryRuntime 由 SessionManager 拥有，不随 Agent 替换
  │  - 所有 SessionEntry 和 ContextEngine 保持不变
  │  - 旧 Agent 在最后一个 shared_ptr 释放后销毁
```

## 7. 流式协议设计

框架使用基于标记（Tag）的流式协议，通过回调函数传递事件：

| 标记 | 格式 | 说明 |
|------|------|------|
| `[STREAM]` | `[STREAM]text...[/STREAM]` | 文本增量（模型输出 token） |
| `[STATUS]` | `[STATUS]message[/STATUS]` | 进度状态（思考、工具调用开始等） |
| `[TOOL_CALLS]` | `[TOOL_CALLS]...[/TOOL_CALLS]` | 工具调用元数据 |
| `[TOOL_RESPONSE <id>]` | `[TOOL_RESPONSE id]...[/TOOL_RESPONSE]` | 工具执行结果 |
| `[ASK_USER]` | `[ASK_USER]...[/ASK_USER]` | 向用户请求澄清（阻塞等待答复） |
| `[FINAL]` | `[FINAL]answer[/FINAL]` | 最终回答 |

应用层（jiuwenClaw）通过 `AgentResponseHandler` 解析这些标记，将不同类型的事件分发到对应的处理逻辑。

## 8. 插件扩展机制

框架提供三个核心扩展点：

### 8.1 模型提供商扩展

```cpp
// 注册自定义模型实现
ResourceManager::GetInstance().RegisterModel(
    "my-provider",
    [](const ModelConfig& cfg) -> std::unique_ptr<Model> {
        return std::make_unique<MyModel>(cfg);
    }
);

// 在 AgentConfig 中指定 provider
config.modelConfig.provider = "my-provider";
```

当 `ModelConfig::provider` 非空时，`ResourceManager::CreateModel()` 优先使用按 provider 注册的工厂；否则按 `formatType`（`OPENAI` / `ANTHROPIC`）查找标准实现。

### 8.2 记忆运行时扩展

**方式一：静态注册**

```cpp
ResourceManager::GetInstance().RegisterMemoryRuntime(
    "my-memory",
    [](const MemoryConfig& cfg) -> std::unique_ptr<MemoryRuntime> {
        return std::make_unique<MyMemoryRuntime>(cfg);
    }
);
```

**方式二：动态插件**

将编译好的 `.so`（Linux）或 `.dll`（Windows）放入插件目录，框架通过 `ResourceManager::LoadMemoryPlugins()` 加载，调用导出的 `RegisterMemoryPlugin(ResourceManager&)` 入口函数。插件使用 `AGENT_PLUGIN_API` 导出宏。

### 8.3 工具扩展

```cpp
// 无状态工具
ResourceManager::GetInstance().RegisterTool(
    "my_tool",
    []() -> std::unique_ptr<Tool> {
        return std::make_unique<MyTool>();
    }
);

// 会话级工具 (通过 ToolBuildContext 注入依赖)
ResourceManager::GetInstance().RegisterSessionTool(
    "my_session_tool",
    [](const ToolBuildContext& ctx) -> std::unique_ptr<Tool> {
        return std::make_unique<MySessionTool>(ctx.todoList, ctx.askUser);
    }
);
```

## 9. 构建体系概述

### 9.1 构建目标

| 目标 | 类型 | 说明 |
|------|------|------|
| `agent_framework` | SHARED | 核心共享库 |
| `jiuwenClaw` | EXECUTABLE | 参考应用 |
| `jiuwenClaw_http_server_adapter` | STATIC | HTTP 适配器 |
| `jiuwenClaw_feishu_adapter` | STATIC | 飞书适配器 |
| `unittest` | EXECUTABLE | 单元测试 |
| `jiuwenClaw_tests` | EXECUTABLE | 应用层测试 |

### 9.2 条件编译

- `JIUWEN_ENABLE_MEMORY_BUILTIN`：启用内置记忆运行时（依赖 `agent-memory-cpp`）
- `BUILDING_AGENT_FRAMEWORK`：控制 `AGENT_API` 的 dllexport/import 方向
- `CPPHTTPLIB_OPENSSL_SUPPORT`：启用 httplib 的 TLS 支持

### 9.3 第三方依赖管理

- 预构建库位于 `libs/` 目录（`libsqlite3.so` / `sqlite3.dll` 等）
- 源码依赖位于 `third_party/src/`（`nlohmann_json`、`sqlite3`、`curl`、`libevent`、`agent-memory-cpp`）
- 头文件位于 `third_party/include/`（`nlohmann/json.hpp`、`httplib.h`、`sqlite3.h`）

## 10. 数据目录布局

运行时数据目录结构（`data/`）：

```
data/
├── agents.json                # Agent 配置覆盖（持久化）
├── channels.json              # 通道配置（jiuwenClaw）
├── mcp_servers.json           # MCP 服务器配置（jiuwenClaw）
├── memory_runtime/
│   ├── memory.db              # 内置记忆运行时数据库
│   └── payloads/              # Offloaded payload 文件
│       └── *.txt
├── sessions/
│   └── <session-id>/
│       ├── context/           # 会话上下文存储
│       │   └── *.json 或 *.db
│       └── ...
├── cron/
│   └── cron_jobs.json         # 定时任务配置
├── heartbeat/
│   └── heartbeat.md           # 心跳任务指令
├── temp/                      # 临时文件
└── skills/                    # 技能目录
    └── <skill-id>/
        └── SKILL.md
```

## 11. 相关文档

| 文档 | 说明 |
|------|------|
| [核心模块设计](core_design.md) | Agent、AgentWorker、AskUserDispatcher、DreamProcessor 等 |
| [会话管理设计](session_design.md) | SessionManager 单例、会话隔离、热重载 |
| [资源管理设计](resource_manager_design.md) | ResourceManager 注册表、两层工具注册、插件加载 |
| [模型适配器设计](model_design.md) | Model 接口、OpenAI / Anthropic 实现 |
| [工具系统设计](tool_design.md) | Tool 基类、内置工具、会话级工具、MCPTool |
| [上下文引擎设计](context_engine_design.md) | ContextEngine、存储后端、上下文压缩 |
| [记忆系统设计](memory_design.md) | MemoryRuntime 接口、Builtin / Http 实现、Payload Offloading |
| [MCP 集成设计](mcp_design.md) | MCPClient、MCPConnection、MCPTool |
| [技能系统设计](skill_design.md) | SkillEngine、SKILL.md 格式、渐进式披露 |
| [配置系统设计](config_design.md) | AgentConfig、ConfigNode、持久化覆盖 |
| [Worker 执行引擎设计](worker_design.md) | ReactAgentWorker、ReAct 循环、取消机制 |

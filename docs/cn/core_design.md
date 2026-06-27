# 核心模块设计文档

## 1. 模块概述

核心模块（`src/core/`）是 Agent 框架的中枢，负责协调各子系统完成推理-行动循环。核心模块包含以下组件：

| 类/结构 | 文件 | 职责 |
|---------|------|------|
| `Agent` | `include/agent.h` | 顶层编排器，Facade 模式 |
| `AgentWorker` | `src/core/agent_worker.h` | 执行循环抽象基类 |
| `ReactAgentWorker` | `src/workers/react_worker.h` | ReAct 循环具体实现 |
| `WorkerEnv` | `src/core/worker_env.h` | 接口隔离层，解耦 Worker 与 Agent |
| `AskUserDispatcher` | `src/core/ask_user_dispatcher.h` | 异步问答调度器 |
| `SessionTodoList` | `src/core/session_todo_list.h` | 会话级任务列表 |
| `HistoryStore` | `src/core/history_store.h` | 追加式 JSONL 历史存储 |
| `DreamProcessor` | `src/core/dream_processor.h` | 遗留记忆整合处理器 |
| `LongTermConsolidator` | `src/memory/long_term_consolidator.h` | 记忆整合抽象接口 |

## 2. Agent 类设计

### 2.1 类关系

`Agent` 是框架的**Facade 类**，协调以下子系统：

```
Agent
 ├── config_ (AgentConfig)
 ├── worker_ (unique_ptr<AgentWorker>)       ← 执行引擎
 ├── skillEngine_ (shared_ptr<SkillEngine>)  ← 技能发现
 ├── historyStore_ (unique_ptr<HistoryStore>) ← 遗留历史
 ├── longTermConsolidator_ (unique_ptr<LongTermConsolidator>) ← 遗留整合
  ├── memoryRuntime_ (MemoryRuntime*)               ← 非拥有，来自 SessionManager（跨热重载存活）
  ├── workerEnv_ (WorkerEnv*)                      ← 非拥有，来自 SessionManager（消环）
 ├── consolidationThread_ (std::thread)      ← 后台整合线程
 ├── contextEngineGetter_ (function)         ← 从 SessionManager 获取 ContextEngine
 └── sessionActivity_ (map<sid, SessionActivity>) ← 会话活跃状态
```

### 2.2 核心方法

#### `Agent::Invoke(sessionId, query, callback)`

主要入口方法，执行流程：

1. 通知会话活跃（`NotifySessionActive`）
2. 通过 `contextEngineGetter_` 获取会话的 `ContextEngine`
3. 将用户消息添加到 `ContextEngine::AddMessage`
4. 调用 `worker_->Invoke(query, contextEngine, callback)` 执行推理循环
5. 通知会话空闲（`NotifySessionIdle`），触发后台整合条件变量
6. 返回最终回答字符串

#### `Agent::ConsolidationLoop()`

后台线程方法，在构造时启动：

1. 等待条件变量（`cv_`），直到有会话变为空闲
2. 创建 Model 实例一次（`CreateModel`），供两条整合路径共享
3. 若 `MemoryRuntime` 已启用，调用 `MemoryRuntime::Consolidate(request, modelClient)`
4. 若 `MemoryRuntime` 未处理，调用 `longTermConsolidator_->Run(model, historyStore)`
5. 循环继续，直到 `running_` 标志变为 false

#### `SessionManager::ProvideUserResponse(requestId, answer)` （原 `Agent::ProvideUserResponse`）

`SessionManager::ProvideUserResponse(requestId, answer)` 通过 `askRequestToSession_` 索引（requestId → sessionId）定位到正确 `SessionEntry` 的 `AskUserDispatcher::ProvideResponse`，唤醒阻塞的 `ask_user` 工具调用。此设计确保 ask_user 请求在 Agent 热重载后仍可路由到正确会话、实现交互可接续。

#### `Agent::Shutdown()`

优雅停止后台 consolidation 线程的公开方法，幂等：

1. 置 `running_ = false`
2. 取消活跃 worker（`worker_->Cancel()`）
3. 唤醒条件变量（`cv_.notify_all()`）使 `ConsolidationLoop` 退出等待
4. `join()` consolidation 线程

析构函数 `~Agent()` 复用 `Shutdown()` 实现。提供独立的公开方法，是为了让应用层
（经由 `SessionManager::Shutdown()`）在进程退出前确定性地排空后台线程，而不必
依赖 `shared_ptr<Agent>` 的析构时序——`GetAgent()` 可能已将强引用交给外部调用方，
其析构时机不可控。`Shutdown()` 可被多次调用：线程一旦 join 完成，后续调用立即返回。

### 2.3 设计要点

- **非拥有 MemoryRuntime**：`memoryRuntime_` 由 `SessionManager` 拥有，Agent 只持有裸指针（non-owning），绝不在 Agent 内释放。生命周期契约：`SessionManager::memoryRuntime_` 是唯一所有者，在 `Initialize()` 中创建一次、热重载时复用、且声明顺序早于 `agent_`/`sessions_`（成员逆序析构 → runtime 最后销毁），并由 `~SessionManager()`/`Shutdown()` 显式先拆 agent_/sessions_ 再销毁 runtime。因此 Agent、ContextEngine 回调、ToolBuildContext 等处的裸指针在其整个有效期内都不会悬空（详见 #5 评审答复）。
- **contextEngineGetter 函数**：Agent 不直接管理 `ContextEngine`，而是通过回调函数从 `SessionManager` 获取。这个回调在 `SessionManager::Initialize` 时通过 `SetContextEngineGetter` 注入。
- **SessionActivity 追踪**：Agent 维护每个会话的活跃状态（`isBusy` 标志），用于 ConsolidationLoop 判断哪些会话空闲需要整合。

## 3. AgentWorker 抽象基类

### 3.1 设计意图

`AgentWorker` 是执行循环的抽象基类，为不同工作模式提供基础设施。当前仅有 `ReactAgentWorker` 实现；`PLAN_AND_EXECUTE` 和 `WORKFLOW` 模式预留但未实现。

### 3.2 类结构

```
AgentWorker
 ├── config_ (AgentConfig)
 ├── toolNames_ (vector<string>)      ← 注册的工具名称
 ├── toolSelector_ (unique_ptr<ToolSelector>) ← 工具选择器
 ├── skillEngine_ (shared_ptr<SkillEngine>)   ← 技能引擎
 ├── workerEnv_ (WorkerEnv*)          ← 接口隔离，非拥有
 ├── cancelGeneration_ (atomic<uint64_t>) ← 取消代数计数器
 │
 ├── 纯虚方法:
 │   Invoke(query, contextEngine, callback)
 │
 ├── 保护方法:
 │   CallModelStream(systemPrompt, messages, onChunk, generation)
 │   BuildToolSchemas()
 │   BuildPrompt(templateName, query, context, contextEngine)
 │   ExecuteTool(toolName, input, streamCallback)
 │   GetToolSchemaForQuery(query)
 │   GetTodoSnippet()
 │   IsCancelled(myGeneration)
 │   CurrentCancelGeneration()
```

### 3.3 CallModelStream

`CallModelStream` 是核心基础设施方法，封装了模型调用流程：

1. 通过 `BuildToolSchemas()` 构建原生 function-calling 工具 schema
2. 调用 `Model::Format()` 生成请求体
3. 调用 `Model::Invoke()` 获取流式响应（`Invoke` 内部自动重试瞬态错误）
4. 返回 `ModelResponse`（包含文本内容 + tool_calls + isFinished 标志 + isRetryable + statusCode）

此方法支持 `generation` 参数，在流式回调中检查取消状态。

**重试职责**：瞬态错误（HTTP 429/5xx、curl timeout/connection）的自动重试由 `Model::Invoke` 内部承担，`CallModelStream` 不做重试。`ModelResponse.isRetryable` 标识该错误是否为瞬态可重试（仅供诊断，`CallModelStream` 不据此重试）。默认重试策略：maxRetries=2, baseDelayMs=400ms, maxDelayMs=3000ms, withJitter=true，最坏额外延迟约 1.2s。

### 3.4 BuildPrompt

提示词组装流程：

1. 从 `config_.promptTemplates` 中查找指定模板
2. 调用 `ResolvePromptResource()` 加载文件或内联文本
3. 替换 `{$KEY}` 占位符：
   - `{$query}` → 用户查询
   - `{$context}` → ContextEngine 上下文
   - `{$memory}` → MemoryRuntime 构建的记忆上下文
   - `{$skills}` → SkillEngine 技能目录
   - `{$todo}` → SessionTodoList 任务片段

### 3.5 ExecuteTool

工具执行分为两条路径：

- **无状态工具**：`ResourceManager::CreateTool(name)` → `Tool::Invoke(input)`
- **会话级工具**：`ResourceManager::CreateSessionTool(name, ToolBuildContext)` → `Tool::Invoke(input)`

`ToolBuildContext` 由 `WorkerEnv` 提供的会话级资源构建：
- `todoList` → `WorkerEnv::GetOrCreateSessionTodoList(sessionId)`
- `askUser` → `WorkerEnv::GetAskUserDispatcher(sessionId)`
- `memoryRuntime` → `WorkerEnv::GetMemoryRuntime()`
- `streamCallback` → 来自调用者的流式回调
- `sessionId` → 当前会话 ID

## 4. WorkerEnv 接口隔离

### 4.1 设计动机

`AgentWorker` 需要访问会话级资源（`SessionTodoList`、`AskUserDispatcher`、`MemoryRuntime`），但直接依赖 `Agent` 类会造成循环依赖（`AgentWorker → WorkerEnv → Agent`）。`WorkerEnv` 接口将这种依赖抽象化，实现由 SessionManager 提供，资源存储在 `SessionEntry` 中，消除循环引用且保证跨热重载存活。

### 4.2 接口定义

```cpp
class WorkerEnv {
public:
    virtual ~WorkerEnv() = default;
    virtual SessionTodoList* GetOrCreateSessionTodoList(const std::string& sessionId) = 0;
    virtual AskUserDispatcher* GetAskUserDispatcher(const std::string& sessionId) = 0;
    virtual MemoryRuntime* GetMemoryRuntime() = 0;
};
```

> `GetAskUserDispatcher` 签名为 `const std::string& sessionId`，因为 `AskUserDispatcher` 是会话级资源（随 `SessionEntry` 存活），而非 Agent 级单例。

### 4.3 实现

`SmWorkerEnv`（定义在 `session_manager.cpp`）通过 `SessionManager` 按 sessionId 查找 `SessionEntry`，获取其 `todoList`、`askUser`，以及全局 `memoryRuntime_`。Agent 持有非拥有 `WorkerEnv*`，由 SessionManager 在 Initialize/ReloadAgent 时通过 `Agent::SetWorkerEnv` 注入。

```cpp
class SmWorkerEnv : public WorkerEnv {
    SessionManager* sm_;
public:
    SessionTodoList* GetOrCreateSessionTodoList(const std::string& sessionId) override {
        // 通过 sm_->sessionMutex_ 查找 SessionEntry->todoList
    }
    AskUserDispatcher* GetAskUserDispatcher(const std::string& sessionId) override {
        // 通过 sm_->sessionMutex_ 查找 SessionEntry->askUser
    }
    MemoryRuntime* GetMemoryRuntime() override {
        return sm_->memoryRuntime_.get();
    }
};
```

依赖链为 `AgentWorker → WorkerEnv(SmWorkerEnv) → SessionManager → SessionEntry`，Agent 不被反向引用。

## 5. AskUserDispatcher 异步问答

### 5.1 设计意图

Agent 在执行过程中可能需要向用户请求澄清（如参数不确定、多选题等）。`AskUserDispatcher` 实现了一种**生产者-消费者**模式的异步问答机制。

### 5.2 工作流程

```
Agent Worker (生产者)                  应用层 (消费者)
      │                                    │
      │  AskUserTool::Invoke()             │
      │  ├── EmitAskUser(requestId,        │
      │  │   payloadJson, callback)        │
      │  │   ├── 注册 Slot (mutex + cv +   │
      │  │   │   optional<answer>)          │
      │  │   └── 发送 [ASK_USER] 标记      │
      │  │       到流式回调                 │
      │  │                                  │
      │  ├── WaitForResponse(requestId,     │
      │  │   timeout)                       │
      │  │   ├── 阻塞在 Slot.cv 上          │
      │  │   └── 超时返回 nullopt           │
      │  │                                  │
      │  │        ProvideResponse(          │
      │  │          requestId, answer)      │
      │  │          ├── 找到对应 Slot       │
      │  │          ├── 设置 answer          │
      │  │          └── 通知 Slot.cv        │
      │  │                                  │
      │  ├── 收到 answer                    │
      │  └── 返回工具结果                   │
      │                                    │
```

### 5.3 Slot 结构

```cpp
struct Slot {
    std::mutex m;
    std::condition_variable cv;
    std::optional<std::string> answer;
    bool done{false};
};
```

每个 `ask_user` 请求创建一个 `Slot`，存储在 `slots_` map 中。`EmitAskUser` 注册并通知，`WaitForResponse` 阻塞等待，`ProvideResponse` 写入并唤醒。

### 5.4 线程安全

- `slotsMu_` 保护 `slots_` map 的并发访问
- 每个 `Slot` 有独立的 `mutex` 和 `condition_variable`
- `TakeSlot` 在消费者写入后移除 Slot，防止内存泄漏

## 6. SessionTodoList 会话级任务列表

### 6.1 设计意图

为每个会话维护一个可变的任务清单，Agent 可以在推理过程中创建、更新和查看任务。任务列表作为系统提示的一部分注入，帮助 Agent 组织和跟踪多步骤工作。

### 6.2 数据结构

```cpp
struct TodoItem {
    std::string content;   // 任务描述
    std::string status;    // 状态（"pending" / "completed" / "in_progress"）
    std::string result;    // 完成时的结果记录
};
```

### 6.3 CRUD 操作

- `Add(content)` → 添加新任务
- `Complete(index, result)` → 标记完成
- `Insert(index, content)` → 在指定位置插入
- `Remove(index)` → 移除任务
- `List()` → 列出所有任务

### 6.4 渲染方式

`AgentWorker::GetTodoSnippet()` 将 TodoList 渲染为 Markdown 片段，通过 `{$todo}` 占位符注入系统提示：

```markdown
## Current Todo List
1. [pending] Analyze the input data
2. [completed] Read configuration file → Found 3 settings
3. [pending] Generate summary
```

### 6.5 生命周期管理

- `SessionEntry` 维护 `todoList`（`unique_ptr<SessionTodoList>`），随会话存活、跨热重载保留
- `WorkerEnv::GetOrCreateSessionTodoList(sessionId)` 通过 SessionManager 查找 SessionEntry
- `TodoCreateTool` / `TodoCompleteTool` / 等会话级工具通过 `ToolBuildContext` 获取指针

## 7. HistoryStore 追加式历史存储

### 7.1 设计意图

`HistoryStore` 是遗留的对话历史存储机制，为 `DreamProcessor` 提供数据源。每个交互条目以 JSONL 格式追加到文件中，通过游标文件追踪处理进度。

### 7.2 数据格式

每行一个 JSON 对象：

```json
{"cursor": 1, "timestamp": "...", "role": "user", "content": "...", "toolsUsed": 0}
{"cursor": 2, "timestamp": "...", "role": "assistant", "content": "...", "toolsUsed": 2}
```

### 7.3 游标文件

- `.cursor`：普通游标，记录已处理到的条目编号
- `.dream_cursor`：Dream 处理游标，记录已整合的条目编号

### 7.4 与 MemoryRuntime 的关系

当 `MemoryRuntime` 启用时，`ContextEngine::AddMessage()` 通过回调将事件发送到 `MemoryRuntime::AppendEvent()`，取代了 `HistoryStore` 的职责。`HistoryStore` 仅在 `MemoryRuntime` 未启用时使用。

## 8. DreamProcessor 遗留记忆整合

### 8.1 设计意图

`DreamProcessor` 是遗留的记忆整合机制，在 `MemoryRuntime` 未启用时使用。它通过两阶段 LLM 调用分析对话历史并更新长期记忆文件。

### 8.2 两阶段流程

**Phase 1 — 分析**

1. 从 `HistoryStore` 加载近期对话历史
2. 读取当前 `MEMORY.md`、`SOUL.md`、`USER.md` 文件
3. 使用 LLM 分析历史与记忆的差异
4. 输出 `DreamFinding` 列表（新事实、修正、技能发现）

```cpp
struct DreamFinding {
    std::string type;       // "new_fact" / "correction" / "skill"
    std::string content;    // 发现内容
    std::string skillName;  // 技能名称（仅 type=="skill" 时有效）
    std::string skillDesc;  // 技能描述
};
```

**Phase 2 — 执行**

1. 将 DreamFinding 列表和当前记忆文件内容组合为执行提示
2. LLM 生成文件编辑指令（`read_file`、`edit_file`、`write_file` 工具调用）
3. `DreamProcessor` 内部执行这些工具调用
4. 更新 `MEMORY.md`、`SOUL.md`、`USER.md` 文件

### 8.3 配置参数

`DreamConfig` 控制整合行为：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `maxBatchSize` | 20 | 每次处理的最大历史条目数 |
| `maxIterations` | 10 | Phase 2 最大工具调用迭代数 |
| `maxToolResultChars` | 16000 | 工具结果截断长度 |
| `historyEntryPreviewMaxChars` | 4000 | 历史条目预览最大字符数 |
| `memoryFileMaxChars` | 32000 | 记忆文件最大字符数 |

### 8.4 与 LongTermConsolidator 的关系

`LongTermConsolidator` 是抽象接口，`LegacyDreamConsolidator` 是其具体实现，封装 `DreamProcessor`。`Agent::ConsolidationLoop` 在 `MemoryRuntime` 未启用时使用此路径。

## 9. 模块间交互图

```
SessionManager ──────────────────────────────────────────────────
  │  拥有 Agent (shared_ptr)                                       │
  │  拥有 MemoryRuntime (unique_ptr)                               │
  │  管理 SessionEntry → ContextEngine                             │
  │                                                                │
  │  Initialize(config)                                            │
  │  ├── 构建 Agent(config)                                        │
  │  ├── SetContextEngineGetter(回调)                              │
  │  ├── SetMemoryRuntime(memoryRuntime_.get())                    │
  │  └── SetupAgentContextRouting()                                │
  │      ├── ContextEngine::SetMemoryContextProvider(              │
  │      │       [=](){ memoryRuntime->BuildContext(...) })        │
  │      └── ContextEngine::SetMemoryEventSink(                    │
  │              [=](event){ memoryRuntime->AppendEvent(event) })  │
  │                                                                │
  │  Invoke(sessionId, message, callback)                          │
  │  ├── 入口内联门控: 等待 !reloading_ + 并发槽位, ++count       │
  │  ├── GetOrCreateSession → ContextEngine                        │
  │  ├── agent_->Invoke(sessionId, message, callback)              │
  │  └── releaseGate(): --count + 通知 concurrency/reload          │
  │                                                                │
  │  ReloadAgent(newConfig)                                        │
  │  ├── 设置屏障，排空并发                                         │
  │  ├── 构建新 Agent                                              │
  │  ├── agent_ = make_shared(newAgent)                            │
  │  ├── Cancel 旧 Agent                                          │
  │  └── 释放屏障                                                  │
  └───────────────────────────────────────────────────────────── │
                                                                 │
Agent ───────────────────────────────────────────────────────── │
  │  Invoke(sessionId, query, callback)                            │
  │  ├── NotifySessionActive(sessionId)                            │
  │  ├── contextEngineGetter_(sessionId) → ContextEngine           │
  │  ├── contextEngine->AddMessage(userMsg)                        │
  │  │   └── memoryEventSink_(event) ──→ MemoryRuntime            │
  │  ├── worker_->Invoke(query, contextEngine, callback)           │
  │  │   ├── ReactLoop(迭代)                                       │
  │  │   │   ├── BuildPrompt → {$memory} ──→ MemoryRuntime         │
  │  │   │   ├── contextEngine->GetContextWindow()                 │
  │  │   │   ├── CallModelStream → Model::Format + Invoke          │
  │  │   │   ├── ExecuteTool → ResourceManager                    │
  │  │   │   │   └── ToolBuildContext (WorkerEnv 提供)             │
  │  │   │   ├── Payload Offloading (可选) ──→ MemoryRuntime       │
  │  │   │   └── contextEngine->AddMessage(observation)            │
  │  │   │       └── memoryEventSink_(event) ──→ MemoryRuntime     │
  │  │   └── 返回最终回答                                          │
  │  ├── NotifySessionIdle(sessionId)                              │
  │  └── 返回回答                                                  │
  │                                                                │
  │  ConsolidationLoop (后台线程)                                   │
  │  ├── cv_.wait() 直到会话空闲                                    │
  │  ├── MemoryRuntime::Consolidate(..., modelClient)               │
  │  │   └── HostMemoryModelClient 适配 Model → MemoryModelClient │
  │  └── 或 LegacyDreamConsolidator::Run(model, historyStore)      │
  └─────────────────────────────────────────────────────────────────
```

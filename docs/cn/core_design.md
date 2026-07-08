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
 ├── longTermConsolidator_ (unique_ptr<LongTermConsolidator>) ← 遗留整合
 ├── memoryRuntime_ (MemoryRuntime*)         ← 非拥有，来自 SessionManager（跨热重载存活）
 ├── historyStore_ (HistoryStore*)           ← 非拥有，来自 SessionManager（跨热重载存活，本地简化退避）
 ├── workerEnv_ (WorkerEnv*)                 ← 非拥有，来自 SessionManager（消环）
 ├── consolidationThread_ (std::thread)      ← 后台整合线程
 ├── contextEngineGetter_ (function)         ← 通过 WorkerEnv 预缓存获取 ContextEngine（不再获取 sessionMutex_）
 ├── sessionActivityMutex_ (mutex, L4)       ← 保护 sessionActivity_
 └── sessionActivity_ (map<sid, SessionActivity>) ← 会话活跃状态
```

> **工具状态归属**：`Agent` 不持有工具状态副本——`AddTools` / `SyncMcpTools` / `GetRegisteredTools` 是纯代理，直接转发到 `AgentWorker`。工具名列表（`toolNames_`）、MCP 归属（`ownedMcpTools_`）、工具选择器（`toolSelector_`）的唯一拥有者是 `AgentWorker`（`toolMutex_` 保护）。工具管理设施放在 `AgentWorker` 基类，3 种 worker 子类（React / Plan&Execute / Workflow）继承共享，只重写循环逻辑；`Agent` 经 `ReloadAgent` 整体替换时新 worker 会被 `AddTools` 重新填充，工具不丢。

> **HistoryStore 所有权与退避语义**：`HistoryStore` 不再由 `Agent` 拥有，而由 `SessionManager` 持有（与 `memoryRuntime_` 同模式：声明在 `agent_`/`sessions_` 前、`Initialize` 创建一次、跨 `ReloadAgent` 复用、`Shutdown` 在 `sessions_` 清空后销毁）。它是**本地简化版 MemoryRuntime 退避**——当 `MemoryRuntime` 未配置或 init 失败时，`SessionManager` 在 `FindOrCreateEntry` 把会话的 `ContextEngine` 事件 sink 路由到 `historyStore_`（否则路由到 `memoryRuntime_`）。路由是**启动时静态决策**（非每调动态退避），避免两存储数据分叉。`HistoryStore` 消费**全量事件流**（`MemoryEvent` 全字段，与 `MemoryRuntime` 对齐），存本地 JSONL 供 `DreamProcessor` 挖掘。`Agent::Invoke` 不再做持久化路由决策——只 `AddMessage`，sink 负责路由。

### 2.2 核心方法

#### `Agent::Invoke(sessionId, query, callback)`

主要入口方法，纯 Facade——不做持久化路由决策，只编排"取 ContextEngine → AddMessage → 调 worker → 返回"。执行流程：

1. 通过 `contextEngineGetter_` 获取会话的 `ContextEngine`（从 WorkerEnv 预缓存读取，不获取 sessionMutex_）
2. 通知会话活跃（`NotifySessionActive`），并构造 RAII guard 保证 `NotifySessionIdle` 在所有退出路径（正常返回 / 异常 / 早退）都执行——避免异常时 `sessionActivity_[sid].isBusy` 永久泄漏
3. 将用户消息 `AddMessage` 到 `ContextEngine`（事件 sink 在 `SessionManager::FindOrCreateEntry` 静态设置：路由到 `memoryRuntime_` 或退避 `historyStore_`，Agent 不参与路由决策）
4. 调用 `worker_->Invoke(query, contextEngine, callback)` 执行推理循环——worker 把 intermediate assistant/tool + final answer 都 `AddMessage` 进 `ContextEngine`，同一 sink 接收全量流
5. RAII guard 析构 → `NotifySessionIdle`（`sessionActivity_` 标记 `isBusy=false`，ConsolidationLoop 轮询检测）
6. 返回最终回答字符串

#### `Agent::CleanupSession(sessionId)`

清理 `sessionActivity_` 中指定会话的条目。由 `SessionManager::RemoveSession` 在删除 SessionEntry 后调用（在 `sessionMutex_`（L2）临界区外，避免锁排序违规：L2 必须释放后才获取 `sessionActivityMutex_`（L4））。`CleanupSession` 在 `sessionActivityMutex_` 保护下执行 `erase(sessionId)`。确保已删除会话的 stale 状态不残留在 `sessionActivity_`，防止内存泄漏和 ConsolidationLoop 误触发。

#### `Agent::ConsolidationLoop()`

后台线程方法，在构造时启动。存在两个重载：**无参重载**让 runtime 自行决定模型来源，可用于 runtime 自决场景；**带参重载**接收显式提供的 `modelClient`，意图明确。当前调用带参重载（意图明确），无参重载可用于 runtime 自决场景。流程如下：

1. 在 `consolidationMutex_`（L3）上 `cv_.wait_for(memoryConfig.idleConsolidationSeconds)`，谓词 `!running_`（仅 `Shutdown` 的 notify 提前退出）；超时后释放 L3
2. **首轮 catch-up**：`firstCycle` 局部变量在首次迭代为 true，绕过下述 `anyIdle` 与 `hasNewActivity_` 两道门，无条件执行一次整合，拾取旧 Agent 遗留的 pending 事件（见下方说明）；之后置 `firstCycle=false`，恢复常态门控。注意 catch-up 仍发生在首个 `cv_.wait_for(memoryConfig.idleConsolidationSeconds)` 轮询周期到期之后（见步骤 1），不抢占启动、不立即执行——最坏延迟 `memoryConfig.idleConsolidationSeconds`（默认 60s）
3. 取 `sessionActivityMutex_`（L4）检查 `sessionActivity_` 有无 `isBusy=false` 的空闲会话；无则 continue，有则继续
4. 检查活动门 `hasNewActivity_`（atomic load，无锁）：若自上次整合以来无会话完成新对话则 continue，跳过 `CreateModel` 与底层 cursor 查询；有则继续
5. 清除 `hasNewActivity_`（clear-before，见下方说明），随后创建 Model 实例一次（`CreateModel`），供两条整合路径共享
6. 若 `MemoryRuntime` 已启用，构造 `MemoryConsolidationRequest` 时把 `config_.memoryConfig.excludedConsolidationSessionIds` 透传到 `request.excludedSessionIds`，再调用 `MemoryRuntime::Consolidate(request, modelClient)`
7. 若 `MemoryRuntime` 未处理，调用 `longTermConsolidator_->Run(model, historyStore)`
8. 循环继续，直到 `running_` 标志变为 false

> **轮询驱动而非事件驱动**：ConsolidationLoop 是**超时轮询**设计——每 `memoryConfig.idleConsolidationSeconds` 醒一次检查空闲会话，`cv_` 仅用于 `Shutdown` 提前退出（谓词 `!running_`）。`NotifySessionIdle` 只在 `sessionActivity_` 标记 `isBusy=false`，**不发 `cv_` 信号**。原因：wait 不能在 `sessionActivityMutex_`（L4）上长 wait（会阻塞 `IsSessionBusy`/`NotifySessionIdle`/`CleanupSession` 等所有 session-activity 操作），故 wait 用独立 `consolidationMutex_`（L3）；若在 L4 下 notify 而 wait 持 L3，是经典 CV mutex 不匹配。且无 per-session idle-since 时间戳，"空闲 N 秒"无法真正强制（轮询间隔即近似窗口）。真事件驱动需加 idle-since 时间戳 + 谓词查 idle 时长 + 最近 deadline 计算，属未来增强，不在当前范围。

> **首轮 catch-up（`firstCycle`）**：`ConsolidationLoop` 用局部 `bool firstCycle`（初始 true）让首次迭代绕过 `anyIdle` 与 `hasNewActivity_` 两道门，无条件执行一次整合。动机：`ReloadAgent` 构造新 Agent 时，旧 Agent 最后一轮可能 `hasNewActivity_=true` 但未及整合就被销毁，新 Agent 的 `hasNewActivity_=false` 且 `sessionActivity_` 为空（sessions 在 SessionManager 侧保留，但未在新 Agent 的 activity 表注册）——两道门都阻断，pending 事件要等下次正常会话完成才被拾取。catch-up 让新 Agent 在首个轮询周期拾取这些遗留事件。`firstCycle` 是局部变量、与线程同寿：ReloadAgent 构造新 Agent → 新线程 → 新 `firstCycle=true`，天然重置，无需成员。**不抢占启动资源**：catch-up 不跳过步骤 1 的 `cv_.wait_for(memoryConfig.idleConsolidationSeconds)`，仍要等首个轮询周期到期才执行（最坏延迟 `memoryConfig.idleConsolidationSeconds`，默认 60s）——刻意不与启动初始化抢资源、不立即触发 LLM/模型构造，启动期资源只服务前台。安全性：启动瞬间无活跃会话（`ReloadAgent` 已 drain 到 `concurrentCount_==0`），绕过 `anyIdle` 不会与前台推理抢模型资源；无 pending 事件时 cursor 早退、不调 LLM，成本仅一次 `CreateModel` + 一次 SQLite cursor 查询。

> **活动门 `hasNewActivity_`**：`NotifySessionIdle` 置 `hasNewActivity_=true`（release 序），表示"有会话刚完成对话、事件已落入存储"。**条件置位**：若 `sessionId` 出现在 `config_.memoryConfig.excludedConsolidationSessionIds` 内（如 `__CRON__`/`__HEARTBEAT__` 等系统机械触发会话），则跳过置位——这些会话的事件已被排除出整合批，让它们唤醒脏标记会稀释"无新对话跳过 CreateModel"优化。**仍更新 `isBusy`**：排除集内的会话仍正常更新 `sessionActivity_[sessionId].isBusy`，保留忙闲追踪，不影响 `anyIdle` 门控（第一道门）。ConsolidationLoop 在通过 `anyIdle` 检查后、构造 Model 之前以 `hasNewActivity_.load()`（acquire 序）做快路径门控：为 false 则 `continue`，**完全跳过 `CreateModel` 与底层 cursor 查询**。这是纯性能提示——正确性仍由 agent-memory-cpp 的 cursor 幂等机制兜底（标记漏置最多延迟一个周期，多置最多多一次 cursor 查询）。清除时机采用 **clear-before**：进入整合分支后立即 `store(false)`，再调 `CreateModel`/`Consolidate`。若新对话在整合执行期间完成，`NotifySessionIdle` 会重新置位、下一周期处理，cursor 保证不漏不重；若整合抛异常，cursor 未推进、事件仍 pending，等下一次新对话触发时重试（后台 best-effort 语义，最坏延迟一个轮询周期）。

> **整合排除集**：`MemoryConfig::excludedConsolidationSessionIds` 是配置驱动的通用排除机制，核心库不硬编码任何保留 ID。应用层在启动时填入自己的系统会话标识（jiuwenClaw 填入 `__CRON__`/`__HEARTBEAT__`），`ConsolidationLoop` 把它透传给 `MemoryConsolidationRequest.excludedSessionIds`，再经 `type_bridge` 传到 agent-memory-cpp 的 `ConsolidationBatchBuilder` 与 `MemoryEventStore::LoadEventsAfterCursor`：前者为非 SQL Store 兜底过滤，后者在 SQL 层加 `NOT IN` 减少行加载。被排除事件仍入库（audit trail）、仍推进 cursor，但不进入 batch、不参与 LLM/规则抽取。这同时是 `NotifySessionIdle` 条件置位的数据源（见上段）。

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
- **SessionActivity 追踪**：Agent 维护每个会话的活跃状态（`isBusy` 标志），用于 ConsolidationLoop 判断哪些会话空闲需要整合。配套的 `hasNewActivity_`（atomic）作为活动门，由 `NotifySessionIdle` 置位、`ConsolidationLoop` 在构造 Model 前清除并据此跳过无新事件时的空转整合（详见 `Agent::ConsolidationLoop` 段说明）。

## 3. AgentWorker 抽象基类

### 3.1 设计意图

`AgentWorker` 是执行循环的抽象基类，为不同工作模式提供基础设施。当前仅有 `ReactAgentWorker` 实现；`PLAN_AND_EXECUTE` 和 `WORKFLOW` 模式预留但未实现。

### 3.2 类结构

```
AgentWorker
 ├── config_ (AgentConfig)
 ├── toolNames_ (vector<string>)      ← 注册的工具名称（唯一拥有，toolMutex_ 保护）
 ├── ownedMcpTools_ (vector<string>)  ← 本 worker 加过的 MCP 工具（SyncMcpTools diff 用）
 ├── toolSelector_ (unique_ptr<ToolSelector>) ← 工具选择器
 ├── skillEngine_ (shared_ptr<SkillEngine>)   ← 技能引擎
 ├── workerEnv_ (WorkerEnv*)          ← 接口隔离，非拥有
 ├── cancelGeneration_ (atomic<uint64_t>) ← 取消代数计数器
 │
 ├── 工具管理（toolMutex_ 保护，Agent 退为纯代理）:
 │   AddTools / RemoveTools / GetToolNames / SyncMcpTools
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

`SmWorkerEnv`（定义在 `session_manager.cpp`）通过预缓存的 `shared_ptr<SessionEntry>` 直接获取 `todoList`、`askUser`、`contextEngine`，以及全局 `memoryRuntime_`。在 Invoke 入口处，SessionManager 调用 `WorkerEnv::SetCurrentEntry(entry)` 预缓存当前会话条目；Invoke 完成后调用 `ClearCurrentEntry()` 清除缓存。这种预缓存模式消除了 SmWorkerEnv 在 Invoke 期间获取 `sessionMutex_` 的需要，遵循锁排序协议（避免 invokeMutex（L3）→ sessionMutex_（L2）反向嵌套）。

```cpp
class SmWorkerEnv : public WorkerEnv {
    SessionManager* sm_;
    static thread_local std::shared_ptr<SessionEntry> tlCurrentEntry_;
public:
    std::shared_ptr<ContextEngine> GetContextEngine(const std::string& sessionId) override {
        // Invoke 期间：从 tlCurrentEntry_ 直接返回（不获取 sessionMutex_）
        // 非 Invoke：回退到 sm_->GetContextEngine(sessionId)
    }
    SessionTodoList* GetOrCreateSessionTodoList(const std::string& sessionId) override {
        // Invoke 期间：从 tlCurrentEntry_->todoList 直接返回
        // 非 Invoke：回退到 sm_->sessionMutex_ 查找
    }
    AskUserDispatcher* GetAskUserDispatcher(const std::string& sessionId) override {
        // Invoke 期间：从 tlCurrentEntry_->askUser 直接返回
        // 非 Invoke：回退到 sm_->sessionMutex_ 查找
    }
    MemoryRuntime* GetMemoryRuntime() override {
        return sm_->memoryRuntime_.get();
    }
    void SetCurrentEntry(std::shared_ptr<SessionEntry> entry) override {
        tlCurrentEntry_ = std::move(entry);
    }
    void ClearCurrentEntry() override {
        tlCurrentEntry_.reset();
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

## 7. HistoryStore 本地简化版 MemoryRuntime 退避

### 7.1 设计意图

`HistoryStore` 是**本地简化版 MemoryRuntime 退避**——当 `MemoryRuntime` 未配置或 init 失败时，作为 `ContextEngine` 事件 sink 的退避目标，存储全量对话事件流（与 `MemoryRuntime` 内容对齐）供 `DreamProcessor` 挖掘整合。功能上与 `MemoryRuntime` 同类（存历史 + dream 生成记忆），实现简化（本地 JSONL 文件，无检索/HTTP）。由 `SessionManager` 拥有（跨 `ReloadAgent` 存活，与 `memoryRuntime_` 同模式）。每个事件以 JSONL 格式追加，游标文件追踪 dream 处理进度。

### 7.2 数据格式

每行一个 JSON 对象，字段与 `MemoryEvent` 对齐（全量事件）：

```json
{"cursor": 1, "timestamp": "2026-07-02T11:12:38Z", "session_id": "...", "role": "user", "content": "..."}
{"cursor": 2, "timestamp": "...", "session_id": "...", "role": "assistant", "content": "...", "tool_call_id": "...", "tool_name": "...", "payload_ref": "..."}
```

### 7.3 游标文件

- `.cursor`：普通游标，记录已追加到的条目编号
- `.dream_cursor`：Dream 处理游标，记录已整合的条目编号

### 7.4 与 MemoryRuntime 的关系

`SessionManager::FindOrCreateEntry` 在创建会话时**静态决策**事件 sink 路由：`MemoryRuntime` 已配置 → sink 路由 `memoryRuntime_->AppendEvent`；未配置/init 失败 → sink 路由 `historyStore_->AppendEvent`（退避）。决策是启动时一次性的（`memoryRuntime_` 跨 reload 存活，路由稳定），**非每调动态退避**——避免两存储数据分叉。`HistoryStore` 消费 `MemoryEvent` 全字段（与 `MemoryRuntime` 对齐），`DreamProcessor` 据此构建全量 ReAct trace（含 toolName/callId/payloadRef）进行整合。

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
  │  ├── cv_.wait_for(memoryConfig.idleConsolidationSeconds) 超时或 Shutdown 唤醒 │
  │  ├── MemoryRuntime::Consolidate(..., modelClient)               │
  │  │   └── HostMemoryModelClient 适配 Model → MemoryModelClient │
  │  └── 或 LegacyDreamConsolidator::Run(model, historyStore)      │
  └─────────────────────────────────────────────────────────────────
```

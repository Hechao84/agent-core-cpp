# 会话管理模块设计文档

## 1. 模块概述

会话管理模块（`include/session_manager.h`、`src/session/session_manager.cpp`）是框架的运行时中枢，负责管理多会话状态、并发控制、Agent 热重载以及会话级资源路由。`SessionManager` 采用全局单例模式，是应用层与框架核心交互的唯一入口。

## 2. SessionManager 类设计

### 2.1 单例模式

```cpp
// 全局单例访问
AGENT_API SessionManager& GetSessionManager();
AGENT_API void InitSessionManager(const AgentConfig& config);
```

- `InitSessionManager` 首次调用时构建并初始化单例
- `GetSessionManager` 返回单例引用（未初始化时行为未定义）

**线程安全与生命周期（init-once，永不 delete）**：

- 单例指针由 `std::atomic<SessionManager*>` 持有。`GetSessionManager` 走
  acquire-load 快路径，与构造时的 release-store 配对，保证调用方观察到
  完整构造的对象；首次创建在 `g_initMutex` 下做双重检查（DCLP），但读侧
  改用 atomic load，因此**不再是数据竞争**。
- **单例一经发布永不删除**：`InitSessionManager` 不再 delete-and-recreate，
  而是幂等地复用同一实例（无实例则创建，有则重新 `Initialize()`）。这样
  `GetSessionManager()` 返回的引用永远不会悬空（消除 use-after-free）。
- 运行时重新配置走 `ReloadAgent()`（仅原子替换内部 `agent_`），**不**重建
  SessionManager 本身。
- 单例对象的内存不显式释放，由进程退出时交还 OS（标准单例惯例）；后台线程
  的优雅停止见 §2.4 `Shutdown()`。

### 2.2 类结构

```
SessionManager
 ├── config_ (AgentConfig)
 ├── initialized_ (bool)
 │
 ├── agent_ (shared_ptr<Agent>)          ← 活跃 Agent，shared_ptr 保证热重载安全
 ├── memoryRuntime_ (unique_ptr<MemoryRuntime>) ← 记忆运行时，跨热重载存活
 │
 ├── sessions_ (map<sid, unique_ptr<SessionEntry>) ← 会话注册表
 ├── sessionMutex_ (mutex)               ← 保护 sessions_ 并发访问
 │
 ├── concurrencyMutex_ (mutex)           ← 全局并发门控
 ├── concurrencyCv_ (condition_variable)
 ├── concurrentCount_ (int)              ← 当前并发 Invoke 数
 ├── maxConcurrent_ (int)                ← 最大并发限制
 │
 ├── reloading_ (bool)                   ← 热重载屏障标志
 ├── reloadCv_ (condition_variable)      ← 热重载等待条件
```

### 2.3 核心职责

| 职责 | 方法 | 说明 |
|------|------|------|
| 初始化 | `Initialize(config)` | 构建 Agent、MemoryRuntime、设置路由 |
| 会话调用 | `Invoke(sid, msg, cb)` | 核心入口，线程安全 |
| 通道调用 | `InvokeChannel(msg, cb)` | 从 ChannelMessage 自动派生会话键 |
| 会话管理 | `GetOrCreateSession` | 查找/创建 SessionEntry + ContextEngine |
| 热重载 | `ReloadAgent(newConfig)` | 原子替换 Agent |
| 取消 | `Cancel()` | 取消当前 Agent 执行 |
| 优雅停机 | `Shutdown()` | 进程退出前停止后台线程（见 §2.4） |
| 会话查询 | `GetSessionIds/Messages/Metadata` | 查询会话状态 |

### 2.4 优雅停机 Shutdown()

```cpp
void SessionManager::Shutdown();   // 幂等
```

进程退出前应显式调用 `Shutdown()`，以便确定性地停止 Agent 的后台
consolidation 线程（调用 `Agent::Shutdown()` → 置 `running_=false` →
唤醒并 `join()` 线程），而非依赖静态析构时序。

- **不删除单例本身**：仅排空 Agent 的后台工作；单例内存仍交进程退出回收。
- **调用顺序约束**：调用方必须先停止所有可能调用 `SessionManager` 的线程
  （heartbeat、cron、channel、HTTP server），再调用 `Shutdown()`，确保此后
  没有线程会再进入 SessionManager。
- **不主动取消在途 Invoke**：若退出瞬间恰有任务在执行，`join()` 会等待其当前
  这轮 Invoke 自然结束后再返回（该等待在静态析构方案下同样存在）。
- 幂等：`Agent::Shutdown()` 可重复调用，线程已 join 后再次调用立即返回。

> jiuwenClaw 参考实现的退出顺序（`main.cpp`）：
> `ConfigWatcher.Stop` → `ChannelService.StopAll` → `HttpServer.Stop` →
> 销毁 `CronWatcher`/`HeartbeatManager`（join 各自线程）→
> `GetSessionManager().Shutdown()` → `return 0`。

## 3. SessionEntry 与会话隔离

### 3.1 SessionEntry 结构

```cpp
struct SessionEntry {
    std::string sessionId;
    std::shared_ptr<ContextEngine> contextEngine;  // 会话独立的上下文引擎
    std::unique_ptr<SessionTodoList> todoList;      // 会话独立的任务列表
    std::unique_ptr<AskUserDispatcher> askUser;     // 会话独立的问答调度器
    std::mutex invokeMutex;                        // 每会话串行锁
    std::atomic<bool> isBusy{false};               // 会话忙碌标志（跨锁读写，需原子）
    std::map<std::string, std::string> metadata;   // 通道、发送者等元数据
};
```

> `todoList` 和 `askUser` 随 `SessionEntry` 存活、由 SessionManager 拥有，与 `contextEngine` 归属一致。
> 这两项会话级资源现在随 `SessionEntry` 存活，与 `contextEngine` 归属一致，在
> Agent 热重载（`ReloadAgent`）后保留。`AskUserDispatcher` 持有 `sessionId_` 和
> `AskUserRouter*`，在 EmitAskUser 时注册 requestId→sessionId 索引，确保用户
> 回答在热重载后仍可路由到正确会话。

`sessions_` 以 `std::shared_ptr<SessionEntry>` 持有每个条目（而非 `unique_ptr`）。
这样 `Invoke` 可在 `sessionMutex_` 下取得一份 `shared_ptr` 拷贝、释放锁后再长时间
持有 `invokeMutex` 执行：即便 `RemoveSession` 在此期间把条目移出 map，`SessionEntry`
（连同其 `invokeMutex`）也不会被销毁，从而消除并发删除导致的 use-after-free。

`isBusy` 为 `std::atomic<bool>`：写发生在持 `invokeMutex` 的 `Invoke` 路径，读发生在
持 `sessionMutex_` 的 `RemoveSession`/`IsSessionBusy`——读写用不同的锁，故必须用原子
类型保证可见性，避免数据竞争。

### 3.2 会话隔离原则

- **独立 ContextEngine**：每个会话拥有自己的上下文引擎，消息不会跨会话泄露
- **独立 invokeMutex**：同一会话的调用串行执行，保证 ContextEngine 读写安全
- **独立 metadata**：存储通道类型、发送者等会话特有信息

### 3.3 会话生命周期

```
首次 Invoke(sessionId)
  │
  ▼
FindOrCreateEntry(sessionId)   // 返回 shared_ptr<SessionEntry>
  │  ├── 若 sessionId 已存在 → 返回已有 SessionEntry
  │  └── 若不存在 → 创建新 SessionEntry
  │      ├── 构建 ContextConfig (基于全局 config + sessionId)
  │      ├── 创建 ContextEngine(config)
  │      ├── ContextEngine::Initialize()
  │      ├── SetupAgentContextRouting() 设置 MemoryRuntime 回调
  │      └── 保存到 sessions_ map
  │
  ▼
会话使用期间
  │  ├── Invoke → 锁内取 shared_ptr<SessionEntry> 拷贝 → 释放 sessionMutex_
  │  │            → acquire invokeMutex → 执行 → release invokeMutex
  │  ├── ContextEngine 独立管理上下文窗口
  │  └── MemoryRuntime 共享但通过 sessionId 区分数据
  │
  ▼
RemoveSession(sessionId)   // 软删除
  │  1. 单段持锁 sessionMutex_：find → 读 isBusy（原子）→ move 出 shared_ptr → erase
  │     （单一 lock_guard 自动解锁，无手动 unlock、无二次解锁 UB、无 TOCTOU）
  │  2. 磁盘删除护栏：对移出的 entry->invokeMutex 做非阻塞 try_lock
  │     ├── 成功（无 in-flight Invoke）→ remove_all(sessionDir)
  │     └── 失败（正忙）→ 跳过磁盘删除，避免与在途 ContextEngine 写并发
  │  3. 局部 shared_ptr 析构：若仍有 in-flight Invoke 持引用，真正析构延后到引用归零
```

> **删除语义（软删除）**：删除操作总是立即从 map 移除并返回成功，不会因会话忙碌而
> 阻塞或拒绝（避免“卡死任务删不掉”的体验问题）。`shared_ptr` 延寿保证不崩溃，
> `try_lock` 护栏避免最危险的“磁盘删除 vs 在途写”并发。
>
> **已知残余风险（低概率）**：同一 sessionId 在删除后立即被重建，可能短暂出现两个
> `SessionEntry` 指向同一磁盘目录并发读写。`RemoveSession` 的唯一入口是 HTTP
> `DELETE /api/sessions/{id}`，正常用法不会对同一会话并发“删除 + 发消息”，故概率极低。
> 若将来需要彻底消除，可改为忙碌拒绝（busy-reject）或强制 cancel 后删除。


## 4. 全局并发门控

### 4.1 设计意图

`AgentConfig::maxConcurrentSessions` 限制同时执行的 Invoke 数量。当设置为 0 时表示无限制；设置为 N 时，最多 N 个会话可以同时执行。

### 4.2 实现机制

并发门控与热重载屏障**内联实现在 `Invoke()` 的入口**（一把 `concurrencyMutex_`
同时管理两者），不再有独立的 Acquire/Release 方法。进入时：

```
Invoke 入口（持 concurrencyMutex_）
  │  ├── reloadCv_.wait(lock, !reloading_)          ← 热重载期间阻塞新 Invoke
  │  ├── 若 maxConcurrent_ > 0:
  │  │   └── concurrencyCv_.wait(lock,
  │  │         !reloading_ && concurrentCount_ < maxConcurrent_)  ← 等待空闲槽位
  │  └── ++concurrentCount_
```

退出时由 `releaseGate` lambda 统一释放（正常返回、异常、提前返回三条路径共用）：

```
releaseGate()（持 concurrencyMutex_）
  │  ├── 若 concurrentCount_ > 0 → --concurrentCount_
  │  ├── concurrencyCv_.notify_all()   ← 唤醒等待槽位的 Invoke
  │  └── reloadCv_.notify_all()        ← 唤醒等待排空的 ReloadAgent
```

> `concurrentCount_` 既作并发计数，也作热重载的 drain 计数：它始终等于“已进入
> 门控、尚未 releaseGate”的 Invoke 数，与 `maxConcurrent_` 是否启用无关。

### 4.3 与热重载屏障的协作

热重载需要排空所有进行中的调用：

```
ReloadAgent(newConfig)
  │  ├── lock(concurrencyMutex_); reloading_ = true   ← 升起屏障
  │  ├── concurrencyCv_.wait(concurrentCount_ == 0)   ← 等待所有 Invoke 排空
  │  ├── ... 构建并原子替换新 Agent ...
  │  ├── reloading_ = false                            ← 落下屏障
  │  └── reloadCv_/concurrencyCv_.notify_all()         ← 释放阻塞的 Invoke
```

由于门控逻辑内联在 `Invoke` 中、且谓词同时包含 `!reloading_`，新 Invoke 在
热重载期间必然阻塞，保证 drain 不被绕过。

## 5. 会话键派生

### 5.1 MakeSessionKey

```cpp
static std::string MakeSessionKey(const std::string& channel, const std::string& chatId);
```

将通道类型和聊天 ID 组合为稳定的会话键，确保同一用户在不同通道的会话不会混淆。

例如：
- `MakeSessionKey("feishu", "user_123")` → `"feishu:user_123"`
- `MakeSessionKey("cli", "default")` → `"cli:default"`

### 5.2 ChannelMessage 路由

```cpp
SessionInvokeResult InvokeChannel(const ChannelMessage& msg, callback);
```

`ChannelMessage` 包含 `channel`、`chatId`、可选的 `sessionId`：

- 若 `msg.sessionId` 非空 → 直接使用
- 否则 → `MakeSessionKey(msg.channel, msg.chatId)` 自动派生

### 5.3 保留会话 ID

```cpp
inline constexpr char kDefaultSessionId[] = "__DEFAULT__";
inline constexpr char kHeartbeatSessionId[] = "__HEARTBEAT__";
inline constexpr char kCronSessionId[] = "__CRON__";
```

| 会话 ID | 用途 |
|---------|------|
| `__DEFAULT__` | 未显式传入 sessionId 时的默认会话 |
| `__HEARTBEAT__` | 心跳任务的专用会话（jiuwenClaw） |
| `__CRON__` | 定时任务的专用会话（jiuwenClaw） |

这些保留 ID 确保内部功能会话不会与用户会话冲突。

## 6. 热重载原子替换

### 6.1 设计意图

`ReloadAgent` 允许在不重启进程的情况下更新 Agent 配置（如模型、提示词、工具列表等），同时保留所有会话的上下文和记忆数据。

### 6.2 安全保障

| 保障 | 实现方式 |
|------|---------|
| 旧 Agent 不会被提前销毁 | `GetAgent()` 返回 `shared_ptr`，调用者持有强引用 |
| 会话上下文不丢失 | `SessionEntry` 和 `ContextEngine` 由 `SessionManager` 管理，不受 Agent 替换影响 |
| MemoryRuntime 不中断 | `memoryRuntime_` 由 `SessionManager` 拥有，不随 Agent 替换 |
| 进行中调用不被打断 | 通过并发门控排空后才执行替换 |
| 新调用不丢失 | 热重载期间新 Invoke 阻塞等待，完成后自动继续 |

### 6.3 完整流程

```
ReloadAgent(newConfig, errorOut)
  │
  │  Step 1: 设置屏障
  │  ├── lock(concurrencyMutex_)
  │  ├── reloading_ = true
  │  ├── concurrencyCv_.notify_all()  ← 阻止新的 Invoke
  │
  │  Step 2: 排空进行中调用
  │  ├── while (concurrentCount_ > 0)
  │  │   └── concurrencyCv_.wait()    ← 等待所有 Invoke 完成
  │
  │  Step 3: 构建新 Agent
  │  ├── try: Agent(newConfig)
  │  ├── catch: 记录错误，reloading_ = false，返回 false
  │
  │  Step 4: 设置新 Agent 的上下文路由
  │  ├── agent->SetContextEngineGetter(回调)
  │  ├── agent->SetMemoryRuntime(memoryRuntime_.get())
  │  ├── SetupAgentContextRouting()
  │
  │  Step 5: 原子替换
  │  ├── oldAgent = agent_             ← 保存旧 Agent 的 shared_ptr
  │  ├── agent_ = make_shared(newAgent) ← 原子替换
  │  ├── oldAgent->Cancel()            ← 取消旧 Agent（终止后台线程）
  │
  │  Step 6: 释放屏障
  │  ├── reloading_ = false
  │  ├── concurrencyCv_.notify_all()   ← 释放阻塞的 Invoke
  │  └── 返回 true
```

### 6.4 调用者视角

```
调用者 A (正在执行 Invoke)
  │  ├── 持有 agent_ 的 shared_ptr (旧 Agent)
  │  ├── 执行完成 → releaseGate() 递减 concurrentCount_
  │  └── shared_ptr 释放后旧 Agent 销毁

调用者 B (等待 Invoke)
  │  ├── Invoke 入口门控被阻塞 (reloading_ == true)
  │  ├── 热重载完成 → 通过门控
  │  ├── GetAgent() 返回新 Agent 的 shared_ptr
  │  └── 正常执行
```

## 7. MemoryRuntime 初始化与路由设置

### 7.1 InitMemoryRuntime

```
InitMemoryRuntime()
  │  ├── 若 memoryConfig.enabled == false → memoryRuntime_ = nullptr
  │  ├── 若 memoryConfig.mode == "sdk" && provider == "builtin.compat"
  │  │   └── ResourceManager::CreateMemoryRuntime(config)
  │  │       └── BuiltinMemoryRuntime(config)
  │  ├── 若 memoryConfig.mode == "server"
  │  │   └── ResourceManager::CreateMemoryRuntime(config)
  │  │       └── HttpMemoryRuntime(config)
  │  └── 其他 provider
  │      └── ResourceManager::CreateMemoryRuntime(config)
  │          └── 自定义插件实现
```

> **生命周期契约（non-owning 裸指针的安全保证）**：`memoryRuntime_`
> （`unique_ptr`）是唯一所有者，仅在 `Initialize()` 中创建一次，热重载
> （`ReloadAgent`）复用同一实例、不重建不销毁。多处持有**非拥有裸指针**指向它：
> `Agent::memoryRuntime_`、各 `ContextEngine` 的记忆回调（按会话捕获）、
> `ToolBuildContext`/`MemoryReadPayloadTool`、`WorkerEnv::GetMemoryRuntime()`。
> 这些裸指针不会悬空，由两点保证：
> 1. **成员声明顺序**：`memoryRuntime_` 声明早于 `agent_`/`sessions_`，成员逆序
>    析构 → runtime 在它们之后销毁（**不得重排这些成员**）。
> 2. **显式拆除顺序**：`~SessionManager()` 与 `Shutdown()` 先停 `agent_`、清
>    `sessions_`（释放捕获 runtime 的回调），最后才 `memoryRuntime_.reset()`。
>
> 经此设计，评审 #5 担心的“Agent 析构导致 MemoryRuntime 悬空”不会发生——
> runtime 的所有权不在 Agent，且其生命周期覆盖所有 Agent 与 in-flight worker。

### 7.2 SetupAgentContextRouting

为每个已有和新建的 `ContextEngine` 设置 MemoryRuntime 回调：

```
SetupAgentContextRouting()
  │  ├── agent_->SetContextEngineGetter(
  │  │     [=](sid) { return GetOrCreateSession(sid)->contextEngine; })
  │  │
  │  ├── agent_->SetMemoryRuntime(memoryRuntime_.get())
  │  │
  │  └── 对每个 SessionEntry:
  │      ├── contextEngine->SetMemoryContextProvider(
  │      │     [=]() { return memoryRuntime_->BuildContext(request); })
  │      └── contextEngine->SetMemoryEventSink(
  │              [=](event) { memoryRuntime_->AppendEvent(event); })
```

这种回调桥接确保：
- `ContextEngine` 不直接依赖 `MemoryRuntime` 类型
- `MemoryRuntime` 通过裸指针捕获，保证跨热重载存活
- 新建的 `ContextEngine` 也会自动获得路由设置

## 8. 线程安全总结

| 共享状态 | 保护机制 | 访问模式 |
|---------|---------|---------|
| `sessions_` | `sessionMutex_` | 读多写少 |
| `concurrentCount_` | `concurrencyMutex_` + `concurrencyCv_` | Invoke 加减 |
| `reloading_` | `concurrencyMutex_` + `reloadCv_` | 热重载专用 |
| `agent_` (shared_ptr) | 原子替换 + shared_ptr 引用计数 | 读多写极少 |
| `memoryRuntime_` | 构造时设置，之后只读；声明早于 agent_/sessions_ 保证最后析构 | 一次写入 |
| SessionEntry::invokeMutex | 每会话独立锁 | 同会话串行 |

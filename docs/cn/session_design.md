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
 ├── sessions_ (map<sid, shared_ptr<SessionEntry>) ← 会话注册表（每个 entry 绑定一个 Agent）
 ├── sessionMutex_ (mutex)               ← 保护 sessions_ + agent_ 并发访问
 │
 ├── concurrencyMutex_ (mutex)           ← 全局并发门控（可选限流）
 ├── concurrencyCv_ (condition_variable)
 ├── concurrentCount_ (int)              ← 当前并发 Invoke 数
 ├── maxConcurrent_ (int)                ← 最大并发限制（0 = 不限）
```

### 2.3 核心职责

| 职责 | 方法 | 说明 |
|------|------|------|
| 初始化 | `Initialize(config)` | 构建 Agent、MemoryRuntime、设置路由 |
| 会话调用 | `Invoke(sid, msg, cb)` | 核心入口，线程安全 |
| 通道调用 | `InvokeChannel(msg, cb)` | 从 ChannelMessage 自动派生会话键 |
| 会话管理 | `GetOrCreateSession` | 查找/创建 SessionEntry + ContextEngine |
| 热重载 | `ReloadAgent(newConfig)` | 优雅退役：不 drain/Cancel，建新 Agent → 旧 Agent 标记 draining |
| 取消 | `Cancel()` | 取消当前 Agent 执行 |
| 优雅停机 | `Shutdown()` | 进程退出前停止后台线程（见 §2.4） |
| 会话查询 | `GetSessionIds/Messages/Metadata` | 查询会话状态 |

### 2.4 优雅停机 Shutdown()

```cpp
void SessionManager::Shutdown();   // 幂等
```

进程退出前应显式调用 `Shutdown()`，以便确定性地停止所有 Agent 的后台
consolidation 线程（`Agent::Shutdown()` → 置 `running_=false` → 唤醒并 `join()`
线程），而非依赖静态析构时序。

- **收集所有存活 Agent**：优雅退役后可能同时存在多个 Agent（活跃 +
  若干 draining 的旧 Agent，旧 Agent 由绑定它的 `SessionEntry::agent`
  引用保活）。`Shutdown` 在 `sessionMutex_` 下收集 `agent_` 与各
  `sessions_` 中 `entry->agent` 的去重 shared_ptr 集合，逐个 `Shutdown`。
  去重保证同一 Agent 不重复 join（`Shutdown` 本身幂等，去重仅减少噪声）。
- **不删除单例本身**：仅 join Agent 后台工作；单例内存仍交进程退出回收。
- **调用顺序约束**：调用方必须先停止所有可能调用 `SessionManager` 的线程
  （heartbeat、cron、channel、HTTP server），再调用 `Shutdown()`，确保此后
  没有线程会再进入 SessionManager。
- **不主动取消在途 Invoke**：`Shutdown` join 的是 consolidation 线程，不中止
  在途回合；在途 Invoke 持有自身 shared_ptr，Agent 在该引用释放前不析构。
  若退出瞬间恰有任务在执行，该 Agent 仍存活至调用方线程停止后自然释放。
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
    std::shared_ptr<Agent> agent;                   // 该 session 绑定的 Agent；ReloadAgent 后旧 Agent 由本引用保活
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
>
> **`agent` 绑定字段**：新 session 创建时（`FindOrCreateEntry` 新建分支）绑定
> 当前活跃 `agent_`，调用方持 `sessionMutex_`，`agent_` 在该锁下被 `ReloadAgent`
> swap，故绑定读到的是一致的活跃 Agent。`Invoke` 通过 `entry->agent` 路由而非
> 全局 `agent_`。存量 session 的下一条新消息时，若 `entry->agent` 处于 draining，
> `Invoke` 在 `sessionMutex_` 下将其重绑到当前活跃 `agent_`，路由切换。旧 Agent
> 的析构由本引用的 shared_ptr 计数驱动：最后一个引用释放 → `~Agent` →
> `Shutdown` join consolidation 线程，自然消亡。

`sessions_` 以 `std::shared_ptr<SessionEntry>` 持有每个条目（而非 `unique_ptr`）。
这样 `Invoke` 可在 `sessionMutex_` 下取得一份 `shared_ptr` 拷贝、释放锁后再长时间
持有 `invokeMutex` 执行：即便 `RemoveSession` 在此期间把条目移出 map，`SessionEntry`
（连同其 `invokeMutex`）也不会被销毁，从而消除并发删除导致的 use-after-free。

`isBusy` 为 `std::atomic<bool>`：写发生在持 `invokeMutex` 的 `Invoke` 路径，读发生在持 `sessionMutex_` 的 `RemoveSession` 和 `IsSessionBusy`。在 `IsSessionBusy` 中，`sessionMutex_`（L2）先释放，再获取 `sessionActivityMutex_`（L4）查询 `sessionActivity_`——遵循锁排序协议（L2 总是释放后才获取 L4+ 锁）。读写用不同的锁，故必须用原子类型保证可见性，避免数据竞争。

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
  │  ├── 若 sessionId 已存在 → 返回已有 SessionEntry（保留其绑定 Agent）
  │  └── 若不存在 → 创建新 SessionEntry
  │      ├── 绑定此刻活跃 Agent：entry->agent = agent_（持 sessionMutex_）
  │      ├── 构建 ContextConfig (基于全局 config + sessionId)
  │      ├── 创建 ContextEngine(config)
  │      ├── ContextEngine::Initialize()
  │      ├── SetupAgentContextRouting() 设置 MemoryRuntime 回调
  │      └── 保存到 sessions_ map
  │
  ▼
会话使用期间
  │  ├── Invoke → 锁内取 shared_ptr<SessionEntry> 拷贝
  │  │            ├─ 若 entry->agent 处于 draining → 重绑到当前活跃 agent_（sessionMutex_ 下）
  │  │            └─ agentPtr = entry->agent（快照本回合使用的 Agent）
  │  │            → 释放 sessionMutex_ → WorkerEnv::SetCurrentEntry(entry) 预缓存
  │  │            → acquire invokeMutex → 执行 → release invokeMutex → WorkerEnv::ClearCurrentEntry()
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
  │  3. 局部 shared_ptr 析构：若仍有 in-flight Invoke 持引用，真正析构延后到引用归零；
  │     entry 释放 agent 引用，若该 agent 是最后一个引用则触发 ~Agent → Shutdown join
  │  4. 释放 sessionMutex_ 后调用 agent->CleanupSession(sessionId)
  │     清理 Agent::sessionActivity_ 中对应条目（避免 stale 状态残留和 ConsolidationLoop 误触发）
  │     CleanupSession 在 sessionActivityMutex_（L4）保护下执行 erase，遵循锁排序协议
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

并发门控**内联实现在 `Invoke()` 的入口**（一把 `concurrencyMutex_`），不再有
独立的 Acquire/Release 方法。进入时：

```
Invoke 入口（持 concurrencyMutex_）
  │  ├── 若 maxConcurrent_ > 0:
  │  │   └── concurrencyCv_.wait(lock, concurrentCount_ < maxConcurrent_)  ← 等待空闲槽位
  │  └── ++concurrentCount_
```

退出时由 `releaseGate` lambda 统一释放（正常返回、异常、提前返回三条路径共用）：

```
releaseGate()（持 concurrencyMutex_）
  │  ├── 若 concurrentCount_ > 0 → --concurrentCount_
  │  └── concurrencyCv_.notify_all()   ← 唤醒等待槽位的 Invoke
```

> `concurrentCount_` 仅作并发计数（与 `maxConcurrent_` 是否启用无关），
> 不再复用为热重载 drain 计数——见 §6 优雅退役。

### 4.3 与热重载的关系

`ReloadAgent` 采用优雅退役（见 §6）：**不 drain、不阻塞新 Invoke、不 Cancel
旧 Agent**。新 Invoke 立即可路由——新 session 或已重绑的存量 session 走新
Agent；仍绑定旧 Agent 的存量 session 的在途调用继续走旧 Agent（由其自身
`invokeMutex` 串行），下一条消息才重绑。因此并发门控不再与热重载耦合，
`reloading_` / `reloadCv_` 字段已移除。

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

核心库只保留 `__DEFAULT__` 作为 `MakeSessionKey`（channel/chatId 都为空时）的 fallback 产物：

```cpp
inline constexpr char kDefaultSessionId[] = "__DEFAULT__";
```

`__HEARTBEAT__` / `__CRON__` 等系统会话标识由应用层自行定义（如 `examples/jiuwenClaw/reserved_sessions.h`），核心库不再硬编码——核心库不主动使用这些 ID，仅在 `RemoveSession` 被动保护时通过 `RegisterReservedSession` API 查询注册集。应用层在启动时调用 `RegisterReservedSession` 注册自己的系统会话，并在 `MemoryConfig::excludedConsolidationSessionIds` 中列出需要从记忆整合排除的 session。

| 会话 ID | 定义位置 | 用途 |
|---------|---------|------|
| `__DEFAULT__` | 核心库 `include/session_manager.h` | `MakeSessionKey` 双空 fallback；核心库主动使用，应用层不应直接引用 `kDefaultSessionId` 常量，建议设置自己的默认 session 常量 |
| `__HEARTBEAT__` | 应用层 `examples/jiuwenClaw/reserved_sessions.h` | 心跳任务的专用会话 |
| `__CRON__` | 应用层 `examples/jiuwenClaw/reserved_sessions.h` | 定时任务的专用会话 |

#### `RegisterReservedSession` API

```cpp
void SessionManager::RegisterReservedSession(std::string id);
```

注册一个 session 为"保留"（不可通过 `RemoveSession` 删除）。核心库构造时自动注册 `__DEFAULT__`；应用层在启动时注册自己的系统会话。`RemoveSession` 内部查 `reservedSessions_` 集合，命中则拒绝删除并打日志。

保留状态与 `MemoryConfig::excludedConsolidationSessionIds` 是两个正交概念：前者保护 session 不被删除，后者控制 session 的事件是否参与记忆整合。两者通常一起配置（一个 session 既是保留又是排除），但语义上互不依赖——例如 `__DEFAULT__` 是保留但**不应**被排除（用户在默认 session 中的对话有整合价值）。

## 6. 热重载优雅退役

### 6.1 设计意图

`ReloadAgent` 允许在不重启进程的情况下更新 Agent 配置（如模型、提示词、工具
列表等），同时保留所有会话的上下文和记忆数据。核心思路：**换模型不打断、
不续上正在进行的回合**——换 Agent 只发生在回合之间，回合内始终单 Agent。

- **终端用户**：一个回合内完全无感（始终旧 Agent 服务该回合），跨回合换模型
  本身合理（用户发新消息时正好用上新模型）。
- **运维**：完全不阻塞（不再 drain）。

### 6.2 安全保障

| 保障 | 实现方式 |
|------|---------|
| 旧 Agent 不会被提前销毁 | `SessionEntry::agent` 持 shared_ptr，存量 session 的在途回合跑完前引用计数不归零 |
| 会话上下文不丢失 | `SessionEntry` 和 `ContextEngine` 由 `SessionManager` 管理，不受 Agent 替换影响 |
| MemoryRuntime 不中断 | `memoryRuntime_` 由 `SessionManager` 拥有，不随 Agent 替换 |
| 进行中调用不被打断 | 不 drain、不 Cancel 旧 Agent；在途回合由旧 Agent 服务到 final answer |
| 新调用立即路由 | 无 reload barrier；新 session 或已重绑的存量 session 直接走新 Agent |
| ask_user 跨 reload 存活 | `AskUserDispatcher` 随 `SessionEntry` 存活（session 级、跨 Agent），answer 到达后旧 Agent 续跑完 |

### 6.3 完整流程

```
ReloadAgent(newConfig, errorOut)
  │
  │  Step 1: 构建新 Agent（先构造，失败则不动旧 Agent）
  │  ├── try: Agent(newConfig)
  │  │   ├── SetMemoryRuntime(memoryRuntime_.get())
  │  │   ├── SetHistoryStore(historyStore_.get())
  │  │   ├── SetWorkerEnv(workerEnv_.get())
  │  │   ├── AddTools(defaultTools)
  │  │   └── SyncMcpTools()
  │  └── catch: 记录错误，返回 false（旧 Agent 原位不动、未被标记 draining）
  │
  │  Step 2: swap（持 sessionMutex_，与 Invoke 的 entry->agent 读/重绑同锁）
  │  ├── config_ = newConfig
  │  ├── maxConcurrent_ = newConfig.maxConcurrentSessions
  │  ├── oldAgent = std::move(agent_)
  │  ├── agent_ = std::move(newAgent)         ← 活跃 Agent 切换为新 Agent
  │  ├── oldAgent->MarkDraining()             ← 旧 Agent 不接新 session 的在途回合
  │  └── SetupAgentContextRouting()           ← contextEngineGetter_ 重绑到新 Agent
  │
  │  Step 3: 旧 Agent 不 Cancel、不显式释放
  │  └── 由绑定它的 SessionEntry::agent 引用计数驱动：
  │      存量 session 的在途回合跑完、entry 被下一条消息重绑或 session 终止时
  │      引用减；最后一个引用释放 → ~Agent → Shutdown join consolidation 线程。
  │      （oldAgent 此处仅释放 ReloadAgent 本地的引用）
  │
  └── 返回 true
```

### 6.4 Agent draining 标志

`Agent` 持有 `std::atomic<bool> draining_`（跨 Agent 对象实例的读写，故必须
原子）。`MarkDraining()` / `IsDraining()` 是 advisory 路由提示，**不改变 `Invoke`
内部行为**——路由层（`SessionManager::Invoke`）已保证 draining Agent 只被它自己
绑定的存量 session 调用，那些调用本就该继续。`draining_` 纯粹是路由层的"是否
需要重绑"判据。

### 6.5 调用者视角

```
调用者 A（存量 session，正在执行 Invoke，绑定旧 Agent）
  │  ├── 持有 entry->agent 的 shared_ptr（旧 Agent）
  │  ├── ReloadAgent 期间继续在旧 Agent 上跑完本回合（不被 Cancel）
  │  └── 回合结束、下一条新消息到达 → Invoke 发现 entry->agent 处于 draining
  │      → 重绑到当前活跃 Agent → 旧 Agent 失去该引用，若已是最后一个引用则析构

调用者 B（新 session，或已重绑的存量 session，发起 Invoke）
  │  ├── FindOrCreateEntry 新建时绑定活跃 agent_（新 Agent）
  │  └── 直接路由到新 Agent，正常执行

运维（调用 ReloadAgent）
  │  ├── 不等 drain、不阻塞 → 立即返回 true
  └── 旧 Agent 驻留至在途回合跑完（含 ask_user 60s 超时窗口），合理延迟析构
```

### 6.6 Cancel 语义

`SessionManager::Cancel()` 取消**活跃 Agent** 的在途 worker（与运维"中止当前
生成"语义一致）。**不扩展**到 draining 的旧 Agent——那些是存量 session 的
在途回合，运维中止语义不应跨 Agent 传染。取消某 session 绑定 Agent 的能力
不在本设计范围（完整 AgentPool 的 per-session cancel 才需要）。

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
| `sessions_` | `sessionMutex_` (L2) | 读多写少 |
| `concurrentCount_` | `concurrencyMutex_` (L1) + `concurrencyCv_` | Invoke 加减 |
| `agent_` (shared_ptr) | `sessionMutex_` 下 swap + shared_ptr 引用计数 | 读多写极少 |
| `SessionEntry::agent` (shared_ptr) | `sessionMutex_`（绑/重绑）；引用计数兜底跨 reload 存活 | 新建时写、Invoke 重绑时写、其余读 |
| `Agent::draining_` | `std::atomic<bool>`（跨 Agent 实例读写） | ReloadAgent 写、Invoke 读 |
| `memoryRuntime_` | 构造时设置，之后只读；声明早于 agent_/sessions_ 保证最后析构 | 一次写入 |
| SessionEntry::invokeMutex | 每会话独立锁 (L3) | 同会话串行 |
| `sessionActivity_` | `sessionActivityMutex_` (L4, Agent 拥有) | ConsolidationLoop/IsSessionBusy/CleanupSession |
| `askRequestToSession_` | `askIndexMutex_` (L4) | ask_user 请求路由 |

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
| 会话查询 | `GetSessionIds/Messages/Metadata` | 查询会话状态 |

## 3. SessionEntry 与会话隔离

### 3.1 SessionEntry 结构

```cpp
struct SessionEntry {
    std::string sessionId;
    std::shared_ptr<ContextEngine> contextEngine;  // 会话独立的上下文引擎
    std::mutex invokeMutex;                        // 每会话串行锁
    bool isBusy{false};                            // 会话忙碌标志
    std::map<std::string, std::string> metadata;   // 通道、发送者等元数据
};
```

### 3.2 会话隔离原则

- **独立 ContextEngine**：每个会话拥有自己的上下文引擎，消息不会跨会话泄露
- **独立 invokeMutex**：同一会话的调用串行执行，保证 ContextEngine 读写安全
- **独立 metadata**：存储通道类型、发送者等会话特有信息

### 3.3 会话生命周期

```
首次 Invoke(sessionId)
  │
  ▼
FindOrCreateEntry(sessionId)
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
  │  ├── Invoke → acquire invokeMutex → 执行 → release invokeMutex
  │  ├── ContextEngine 独立管理上下文窗口
  │  └── MemoryRuntime 共享但通过 sessionId 区分数据
  │
  ▼
RemoveSession(sessionId)
  │  ├── 移除 SessionEntry
  │  ├── ContextEngine::Clear() 清除数据
  │  └── 删除会话数据文件
```

## 4. 全局并发门控

### 4.1 设计意图

`AgentConfig::maxConcurrentSessions` 限制同时执行的 Invoke 数量。当设置为 0 时表示无限制；设置为 N 时，最多 N 个会话可以同时执行。

### 4.2 实现机制

```
AcquireConcurrency()
  │  ├── lock(concurrencyMutex_)
  │  ├── while (concurrentCount_ >= maxConcurrent_ && maxConcurrent_ > 0)
  │  │   └── concurrencyCv_.wait()
  │  └── concurrentCount_++

ReleaseConcurrency()
  │  ├── lock(concurrencyMutex_)
  │  ├── concurrentCount_--
  │  └── concurrencyCv_.notify_all()
```

### 4.3 与热重载屏障的协作

热重载需要排空所有进行中的调用：

```
ReloadAgent(newConfig)
  │  ├── lock(concurrencyMutex_)
  │  ├── reloading_ = true
  │  ├── while (concurrentCount_ > 0)
  │  │   └── concurrencyCv_.wait()  ← 等待所有 Invoke 完成
  │  ├── ... 构建新 Agent ...
  │  ├── reloading_ = false
  │  └── concurrencyCv_.notify_all() ← 释放阻塞的 Invoke
```

`AcquireConcurrency` 在获取许可前也会检查 `reloading_` 标志：

```
AcquireConcurrency()
  │  ├── while (reloading_)
  │  │   └── concurrencyCv_.wait()  ← 热重载期间阻塞新 Invoke
  │  ├── ... 正常获取许可 ...
```

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
  │  ├── 执行完成 → ReleaseConcurrency()
  │  └── shared_ptr 释放后旧 Agent 销毁

调用者 B (等待 Invoke)
  │  ├── AcquireConcurrency() 被阻塞 (reloading_ == true)
  │  ├── 热重载完成 → 获取许可
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
| `memoryRuntime_` | 构造时设置，之后只读 | 一次写入 |
| SessionEntry::invokeMutex | 每会话独立锁 | 同会话串行 |

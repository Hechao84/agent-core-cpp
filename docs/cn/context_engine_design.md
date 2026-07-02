# 上下文引擎设计文档

## 1. 模块概述

上下文引擎模块（`src/context_engine/`）负责管理每个会话的对话上下文窗口，包括消息存储、token 估算、上下文裁剪和压缩。它是 Agent 推理循环的核心数据支撑，确保模型在有限的上下文窗口内获得最相关的信息。

## 2. ContextEngine 类设计

### 2.1 类结构

```
ContextEngine
 ├── config_ (ContextConfig)
 ├── memoryBuffer_ (vector<Message>)     ← 内存中的消息缓冲区
 ├── storage_ (unique_ptr<ContextStorageBase>) ← 持久化存储后端
 ├── memoryContextProvider_ (function<string()>) ← MemoryRuntime 上下文回调
 ├── memoryEventSink_ (function<void(MemoryEvent)>) ← MemoryRuntime 事件回调
 ├── memoryMutex_ (mutex)               ← 保护 memoryBuffer_ 及两个回调对象的并发访问
```

### 2.2 核心方法

| 方法 | 职责 |
|------|------|
| `Initialize()` | 初始化存储后端 |
| `AddMessage(msg)` | 添加消息 + 持久化 + 发送事件 |
| `GetContextWindow()` | 获取经过裁剪的上下文消息 |
| `GetContextAsString()` | 获取上下文的文本表示 |
| `GetAllMessages()` | 获取所有原始消息 |
| `BuildMessagesForLLM()` | 构建发送给 LLM 的消息列表 |
| `Clear()` | 清除所有消息和存储 |
| `GetTokenCount()` | 估算当前 token 总数 |
| `SetMemoryContextProvider()` | 设置记忆上下文提供回调 |
| `SetMemoryEventSink()` | 设置记忆事件接收回调 |
| `GetConsolidationPayload()` | 获取整合所需的消息数据 |

## 3. 消息存储与持久化

### 3.1 双层存储

ContextEngine 采用**内存 + 持久化**双层存储：

- `memoryBuffer_`：内存中的 `vector<Message>`，用于快速访问和上下文裁剪
- `storage_`：持久化存储后端（可选），用于消息持久化和恢复

### 3.2 AddMessage 流程

```
AddMessage(message)
  │  1. lock(memoryMutex_)
  │  2. memoryBuffer_.push_back(message)
  │  3. 若 storage_ 存在 → storage_->SaveMessage(message)
  │  4. 若 memoryEventSink_ 存在:
  │     ├── 构建 MemoryEvent:
  │     │   ├── type = MESSAGE_APPENDED
  │     │   ├── sessionId = config_.sessionId
  │     │   ├── role = message.role
  │     │   ├── content = message.content
  │     │   ├── toolCallId / toolName / payloadRef (如有)
  │     │   └── timestamp = 当前时间
  │     └── 将 memoryEventSink_ 拷贝到局部变量 sink
  │  5. unlock
  │  6. 若 sink 存在 → sink(event) → MemoryRuntime::AppendEvent(event)（锁外调用）
```

> **并发设计说明**：`memoryEventSink_` 在 HTTP 记忆模式下会执行网络 I/O。
> 若在持有 `memoryMutex_` 期间调用，临界区时长将被网络延迟绑定，阻塞所有
> 读者（consolidation 线程、外部查询）。因此采用「锁内拷贝回调 → 锁外执行」
> 模式：既消除对 `memoryBuffer_` 与回调对象的数据竞争，又避免锁内做阻塞 I/O，
> 同时防止未来回调重入 ContextEngine 造成自死锁。


### 3.3 消息恢复

当会话首次访问时（或从磁盘恢复），`Initialize()` 从存储后端加载历史：

```
Initialize()
  │  1. 若 storage_ 存在:
  │     ├── storage_->LoadHistory(outMessages)
  │     ├── memoryBuffer_ = outMessages
  │  2. 返回 true
```

## 4. 存储后端

### 4.1 ContextStorageBase 抽象接口

```cpp
class ContextStorageBase {
public:
    explicit ContextStorageBase(const std::string& sessionId);
    virtual ~ContextStorageBase() = default;

    virtual bool SaveMessage(const Message& msg) = 0;
    virtual bool LoadHistory(std::vector<Message>& outMessages) = 0;
    virtual void Clear() = 0;

    static bool IsValidMessage(const Message& msg);
    static std::string CleanMessageContent(const std::string& input);
protected:
    std::string sessionId_;
};
```

### 4.2 三种存储类型

`ContextConfig::StorageType` 决定使用哪种后端：

| 类型 | 类 | 存储位置 | 特点 |
|------|-----|---------|------|
| `MEMORY_ONLY` | 无（`storage_ = nullptr`） | 仅内存 | 临时性，进程退出丢失 |
| `JSON_FILE` | `JsonStorage` | `data/sessions/<sid>/context/*.json` | 默认，人类可读 |
| `DATABASE` | `DbStorage` | `data/sessions/<sid>/context/<sid>.db` | 高效，适合长会话 |

### 4.3 后端选择策略

```
Initialize()
  │  ├── MEMORY_ONLY → storage_ = nullptr
  │  ├── JSON_FILE → storage_ = make_unique<JsonStorage>(sessionId_, storagePath)
  │  ├── DATABASE → storage_ = make_unique<DbStorage>(sessionId_, storagePath)
```

### 4.4 JsonStorage

- 每条消息保存为独立 JSON 文件（按序号命名）
- `LoadHistory` 读取目录中所有 JSON 文件并按序排序
- 使用 nlohmann/json 序列化/反序列化

### 4.5 DbStorage

- 使用捆绑的 SQLite3 库
- 单表存储所有消息（按时间戳排序）
- 支持 WAL 模式，并发读写安全

## 5. 上下文窗口管理

### 5.1 设计意图

LLM 有上下文窗口限制（token 数量）。`GetContextWindow()` 从完整的消息历史中选取最相关的部分，确保不超过 `config_.maxContextTokens` 和 `config_.maxMessages` 限制。

### 5.2 GetContextWindow 流程

```
GetContextWindow()
  │  1. lock(memoryMutex_) → 拷贝 memoryBuffer_ 副本 → unlock
  │  2. 合并相邻同类消息（在副本上进行，无需持锁）
  │     ├── 连续 user+user → 合为一条
  │     ├── 连续 text-only assistant+assistant → 合为一条
  │     ├── 含 tool_calls / tool_call_id 的消息不可合并
  │  3. 清理孤立消息
  │     ├── DropUnpairedToolMessages: 移除无匹配 tool_call_id 的 tool 消息
  │     ├── DropOrphanToolCalls: 移除无对应 tool result 的 assistant tool_calls
  │     ├── TrimOrphanTrailingToolCalls: 移除末尾的孤立 tool_calls
  │  4. ApplyContextLimits → 消息分段 + 压缩
  │  5. 返回裁剪后的消息列表
```

### 5.3 CanMerge 规则

```cpp
static bool CanMerge(const Message& prev, const Message& cur);
```

| prev.role | cur.role | prev 有 tool_calls/tool_callId? | 可合并? |
|-----------|----------|-------------------------------|---------|
| user | user | — | ✅ |
| assistant | assistant | 任一有 | ❌ |
| assistant | assistant | 都无 | ✅ |
| 其他组合 | — | — | ❌ |

合并内容：`prev.content + "\n" + cur.content`

### 5.4 孤立消息清理

对话中可能出现不完整的工具调用序列：

- **孤立 tool 消息**：有 `toolCallId` 但无对应的 assistant `tool_calls`
  - 原因：上下文裁剪移除了 assistant 消息但保留了 tool result
  - 处理：移除这些 tool 消息

- **孤立 tool_calls**：assistant 有 `tool_calls` 但无对应的 tool result
  - 原因：上下文裁剪移除了 tool result 但保留了 assistant 消息
  - 处理：移除这些 tool_calls 条目（保留 assistant 的文本内容）

- **末尾孤立 tool_calls**：最后一条 assistant 消息只含 tool_calls 无文本
  - 原因：循环未完成，模型还在等待工具结果
  - 处理：移除末尾的这类消息

## 6. 消息分段与上下文压缩

### 6.1 MessageSegment

```cpp
struct MessageSegment {
    int start;              // 起始消息索引
    int end;                // 结束消息索引
    bool startsWithUser;    // 是否以用户消息开头
    int tokens;             // 估算 token 数
};
```

分段以**用户消息为边界**：每个新用户消息开始一个新段。

### 6.2 BuildMessageSegments

```
BuildMessageSegments(messages)
  │  1. 遍历消息列表
  │  2. 每遇到 role == "user" → 开始新段
  │  3. 计算每段的 token 数 (CalculateMessagesTokens)
  │  4. 返回 vector<MessageSegment>
```

### 6.3 ApplyContextLimits 算法

```
ApplyContextLimits(messages)
  │  1. 计算总 token 数
  │  2. 若总 token ≤ maxContextTokens → 返回原始消息
  │  3. BuildMessageSegments(messages)
  │  4. 从最新段开始，保留尽可能多的完整段
  │     ├── budget = maxContextTokens
  │     ├── 对每个段（从最新到最旧）:
  │     │   ├── 若段 token ≤ budget → 保留，budget -= tokens
  │     │   └── 若段 token > budget → 停止保留
  │  5. 对超出预算的旧段:
  │     ├── 若 enableSummarization → CompressSegment(压缩)
  │     └── 否则 → 移除
  │  6. 检查 maxMessages 限制，裁剪超出数量的旧消息
  │  7. 返回裁剪后的消息列表
```

### 6.4 CompressSegment

当 `config_.enableSummarization = true` 时，超出 token 预算的旧段被压缩。

**双重条件守卫**：仅当段**同时**超 token 预算（`CalculateMessagesTokens > tokenBudget`）**且**超消息数上限（`size > config_.maxMessages`）时才压缩；否则原样返回。这避免了对"仅超一项"的段做不必要的摘要化。

```
CompressSegment(messages, segment, tokenBudget)
  │  0. 双重条件守卫: 若 tokens(segment) <= tokenBudget 且 size <= maxMessages → 原样返回
  │
  │  1. 保留段首 user 消息（若段首是 user）作为压缩后的第一条
  │
  │  2. 提取段中所有 tool 消息，每条压缩为一行摘要:
  │     "- {toolName} payload_ref={ref}: {content 前 240 字符预览}..."
  │     （最多保留 8 条，超出加 "...")
  │
  │  3. 提取最近一条无 tool_calls 的 assistant 消息文本（前 800 字符预览）
  │
  │  4. 组合为一条 assistant 摘要消息:
  │     "Context segment compressed due to context window limits.
  │      Tool results summary: ...
  │      Latest assistant state: ..."
  │
  │  5. 压缩结果 = [段首 user?, 摘要消息]（0-2 条）
  │
  │  6. 裁剪到 tokenBudget（保底机制，见下）
```

**裁剪保底机制**：若压缩结果仍超 `tokenBudget`，**不**用 `pop_back` 丢弃摘要消息（那样会塌缩成只剩段首 user 消息、丢失全部 tool/assistant 上下文）。改为**截断摘要消息的 content** 至 `tokenBudget - 段首 user 的 token 数`：循环对半截断 `summary.content` 直到 `EstimateTokens <= budgetForSummary` 或内容为空；内容截断到空时降级为 `"[compressed]"` 占位符。这保证极小预算下仍保留一个（截断的）上下文标记，而非完全丢失。

> **极端低预算行为**：当 `tokenBudget` 极小（远小于段首 user 消息本身）时，`budgetForSummary = max(1, tokenBudget - prefixTokens)` 可能为 1，摘要被截断到接近空 + `[compressed]` 占位。结果为 `[段首 user, "[compressed]"]`——模型至少知道此前有被压缩的上下文，而非只见一条孤立的 user 消息。

### 6.5 Token 估算

```cpp
static int EstimateTokens(const std::string& text);
```

按 **UTF-8 字符类型加权**估算，区分 ASCII 与多字节字符：

- 逐字节遍历 UTF-8 串，按首字节判定每个码点占用的字节数（1/2/3/4 字节）。
- ASCII（单字节）累计后按 **约 1/4 token/字符** 计费。
- 非 ASCII 码点（≥2 字节，如 CJK）按 **约 3/4 token/字符** 计费（略微高估，
  使裁剪倾向于“宁可少装”，避免超出模型上下文窗口）。
- 即 `tokens ≈ asciiChars/4 + wideChars*3/4`。

> **为何不再用 `length()/4`**：旧公式按字节数估算，一个中文字符占 3 字节、
> 实际约 0.6–1 token，`/4` 会低估 3–4 倍，导致裁剪器误判“未超限”而放行
> 过量中文内容，实际发给模型时溢出。加权估算修正了这一偏差。
> 该估算仍是启发式、不依赖外部分词器（不引入 tiktoken/icu 重依赖），但足以
> 做预算分配。

```cpp
int CalculateMessageTokens(const Message& msg) const;
```

计算单条消息的 token 数，包括：
- `content` 文本
- `toolCalls` 的 name + arguments
- `toolCallId` + `toolName`（tool 消息）
- 固定开销（角色标记、格式标记等）

## 7. 与 MemoryRuntime 的回调桥接

### 7.1 设计意图

ContextEngine 不直接依赖 `MemoryRuntime` 类型，而是通过两个回调函数桥接：

- `memoryContextProvider_`：在系统提示组装时提供长期记忆上下文
- `memoryEventSink_`：在消息添加时发送事件到记忆系统

### 7.2 memoryContextProvider

```cpp
void SetMemoryContextProvider(std::function<std::string()> provider);
```

在 `AgentWorker::BuildPrompt` 中，`{$memory}` 占位符的替换调用此回调：

```
BuildPrompt → RenderPrompt
  │  ├── "{$memory}" → LoadMemoryContent()
  │  │   ├── 若 memoryContextProvider_ 存在 → provider() → MemoryRuntime::BuildContext()
  │  │   └── 否则 → 返回空字符串
```

`MemoryRuntime::BuildContext()` 返回 `MemoryContextPackage`，其中 `memoryText` 字段包含长期记忆的文本摘要，被注入到系统提示中。

### 7.3 memoryEventSink

```cpp
void SetMemoryEventSink(std::function<void(const MemoryEvent&)> sink);
```

在 `AddMessage` 中调用，将对话事件发送到 `MemoryRuntime::AppendEvent()`：

```
AddMessage(message)
  │  ├── lock(memoryMutex_)
  │  │   ├── memoryBuffer_.push_back(message)
  │  │   ├── storage_->SaveMessage(message)
  │  │   └── 拷贝 memoryEventSink_ 到局部 sink
  │  ├── unlock
  │  └── sink(MemoryEvent{...}) → MemoryRuntime::AppendEvent()（锁外调用）
```

### 7.4 SetupAgentContextRouting

`SessionManager::SetupAgentContextRouting()` 为每个 ContextEngine 设置这两个回调：

```cpp
contextEngine->SetMemoryContextProvider([=]() {
    return memoryRuntime_->BuildContext(request).memoryText;
});

contextEngine->SetMemoryEventSink([=](const MemoryEvent& event) {
    memoryRuntime_->AppendEvent(event);
});
```

回调通过裸指针 `memoryRuntime_` 捕获，确保跨 Agent 热重载存活。

## 8. BuildMessagesForLLM

### 8.1 设计意图

`BuildMessagesForLLM` 将系统提示、历史消息和当前消息组合为发送给 LLM 的完整消息列表，处理角色顺序约束。

### 8.2 处理规则

不同 LLM 提供商对消息格式有不同要求：

- **OpenAI**：`system` → `user` → `assistant` → `tool` 交替
- **Anthropic**：必须 `user` / `assistant` 交替，不允许连续同角色

`BuildMessagesForLLM` 负责确保消息序列符合这些约束。

## 9. 配置参数

### 9.1 ContextConfig 完整字段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `maxContextTokens` | int | 4096 | 最大上下文 token 数 |
| `maxMessages` | int | 50 | 最大消息条数 |
| `sessionId` | string | - | 会话 ID |
| `storagePath` | string | - | 存储路径 |
| `storageType` | StorageType | JSON_FILE | 存储后端类型 |
| `enableSummarization` | bool | false | 是否启用旧消息压缩 |
| `idleConsolidationSeconds` | int | 60 | 空闲整合触发秒数 |

### 9.2 会话数据路径

```
MEMORY_ONLY → 无持久化
JSON_FILE → data/sessions/<sessionId>/context/
DATABASE → data/sessions/<sessionId>/context/<sessionId>.db
```

`DataDir::SessionDataPath(sessionId)` 提供标准路径。

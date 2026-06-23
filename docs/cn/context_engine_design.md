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
 ├── memoryMutex_ (mutex)               ← 保护内存缓冲区并发访问
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
  │  4. 若 memoryEventSink_ 存在 → memoryEventSink_(event)
  │     ├── 构建 MemoryEvent:
  │     │   ├── type = MESSAGE_APPENDED
  │     │   ├── sessionId = config_.sessionId
  │     │   ├── role = message.role
  │     │   ├── content = message.content
  │     │   ├── toolCallId / toolName / payloadRef (如有)
  │     │   └── timestamp = 当前时间
  │     └── memoryEventSink_ → MemoryRuntime::AppendEvent(event)
  │  5. unlock
```

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
  │  1. 获取 memoryBuffer_ 的副本
  │  2. 合并相邻同类消息
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

当 `config_.enableSummarization = true` 时，超出 token 预算的旧段被压缩：

```
CompressSegment(messages, segment, tokenBudget)
  │  1. 提取段中的所有工具调用和结果
  │  2. 将每个工具调用压缩为一行摘要:
  │     "Called {toolName}({args摘要}) → {result摘要}"
  │  3. 组合为一条 assistant 消息:
  │     "Previous actions summary:\n- Called read_file(/tmp/a) → Found 3 lines\n- ..."
  │  4. 替换原始的多条消息
  │  5. 返回压缩后的消息列表
```

### 6.5 Token 估算

```cpp
static int EstimateTokens(const std::string& text);
```

使用简单的字符数 / 4 估算（英文约 4 字符 = 1 token，中文约 2 字符 = 1 token）。此估算不精确但足以做预算分配。

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
  │  ├── memoryBuffer_.push_back(message)
  │  ├── storage_->SaveMessage(message)
  │  └── memoryEventSink_(MemoryEvent{...}) → MemoryRuntime::AppendEvent()
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

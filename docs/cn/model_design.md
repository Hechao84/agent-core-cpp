# 模型适配器设计文档

## 1. 模块概述

模型适配器模块（`include/model.h`、`src/models/`）封装了与不同 LLM 提供商的交互协议，将厂商特定的 API 格式抽象为统一的 `Model` 接口。当前支持 OpenAI 和 Anthropic 两种格式，通过 provider 扩展机制可接入更多厂商。

## 2. Model 抽象接口

### 2.1 类定义

```cpp
class Model {
public:
    explicit Model(ModelConfig config);
    virtual ~Model() = default;

    // 格式化请求体（厂商特定）
    virtual std::string Format(const std::string& systemPrompt,
                                const std::vector<Message>& messages,
                                const std::vector<ToolSchema>& tools) = 0;

    // 执行推理调用（流式）
    virtual ModelResponse Invoke(const std::string& formattedInput,
                                  std::function<void(const std::string&)> onChunk) = 0;

    ModelConfig GetConfig() const;
protected:
    ModelConfig config_;
};
```

### 2.2 两个核心方法

| 方法 | 职责 | 输入 | 输出 |
|------|------|------|------|
| `Format()` | 将对话上下文序列化为厂商特定的请求 JSON | 系统提示 + 消息列表 + 工具 Schema | JSON 字符串 |
| `Invoke()` | 发送请求体到 API，接收流式响应 | 格式化后的请求体 + 流式回调 | `ModelResponse` |

这种 **Format + Invoke** 的两阶段设计使得：
- `Format` 可被单独测试和调试（查看生成的请求体）
- `Invoke` 只处理 HTTP 通信和响应解析
- 两者解耦，便于扩展新格式

### 2.3 策略模式

`Model` 使用**策略模式**：不同提供商实现不同的 `Format` 和 `Invoke` 策略。`ResourceManager` 根据配置选择合适的策略实例。

**实例生命周期**：`Model` 对象是**轻量配置持有者**（仅持有 `ModelConfig` + `RetryPolicy`，无连接状态、无可变运行时数据）。`AgentWorker::CallModelStream` 每次调用通过 `ResourceManager::CreateModel` 新建一个 `Model` 实例，函数返回时析构——`Model` 是 per-call 短生命周期对象。这是合理的：真正的连接状态（CURL handle、TCP/TLS/DNS 缓存）不在 `Model` 内，而在 `CurlClient` 的 thread_local 持久化 handle 中（见 `docs/cn/curl_client_design.md`），因此 per-call 新建 `Model` 不会触发 `curl_easy_init`，同线程连续调用复用同一连接。故无需缓存 `Model` 对象（详见 `worker_design.md` §5.4）。

## 3. 核心数据结构

### 3.1 ToolCall

```cpp
struct ToolCall {
    std::string id;              // 工具调用唯一标识
    std::string name;            // 工具名称
    std::string argumentsJson;   // JSON 编码的参数
};
```

- **原生模式**：`id` 由 API 返回，用于关联 assistant tool_call 和 tool result
- **回退模式**：`id` 由框架生成，确保消息链完整

### 3.2 ToolSchema

```cpp
struct ToolSchema {
    std::string name;
    std::string description;
    nlohmann::json parameters;   // JSON Schema 对象
};
```

描述一个工具的完整参数规范，发送给模型使其知道可调用的工具。

### 3.3 Message

```cpp
struct Message {
    std::string role;            // system / user / assistant / tool
    std::string content;         // 文本内容
    std::vector<ToolCall> toolCalls;  // assistant 消息的工具调用列表
    std::string toolCallId;      // tool 消息关联的工具调用 ID
    std::string toolName;        // tool 消息的工具名称（信息性）
    std::string payloadRef;      // 可选的 offloaded payload 引用
};
```

`payloadRef` 是 Payload Offloading 的关键：当工具结果过大时，实际内容被写入 MemoryRuntime，上下文中只保留简短摘要 + payloadRef。模型可通过 `memory_read_payload` 工具按需读取完整内容。

### 3.4 ModelResponse

```cpp
struct ModelResponse {
    std::string content;              // 文本内容
    std::vector<ToolCall> toolCalls;  // 工具调用列表
    bool isFinished{false};           // 是否完成（无更多工具调用）
    std::string finishReason;         // "stop" / "tool_calls" / "length" / "error"
    bool isRetryable{false};          // 仅 finishReason=="error" 时有效：瞬态错误可重试
    std::optional<int> statusCode;    // HTTP 状态码（诊断用途）；连接层失败/非HTTP Model 留空
};
```

`isRetryable` 与 `finishReason` 的联动约定：
- `isRetryable=true` ⟹ `finishReason` 必为 `"error"`
- `isRetryable=false` ⟹ 可能是成功（`"stop"`/`"tool_calls"`/`"length"`）或不可重试错误（`"error"`）
- `statusCode` 仅作诊断日志用途，重试决策不依赖它；非 HTTP Model 实现（如本地推理）statusCode 留空

### 3.5 ModelConfig

```cpp
struct RetryPolicy {
    int maxRetries{2};       // 重试次数（总尝试 = 1 + maxRetries）
    int baseDelayMs{400};    // exponential backoff 基础延迟
    int maxDelayMs{3000};    // 退避上限
    bool withJitter{true};   // 随机抖动避免 thundering herd
};

struct ModelConfig {
    std::string baseUrl;              // API 端点地址
    std::string apiKey;               // API 认证密钥
    std::string modelName;            // 模型名称
    std::string provider;             // 自定义提供商标识（可选）
    ModelFormatType formatType;       // API 协议格式（OPENAI / ANTHROPIC）
    bool useNativeFunctionCalling;    // 是否使用原生 function calling
    ConfigNode extraParams;           // 扩展参数（temperature, max_tokens 等）
    RetryPolicy retryPolicy;          // 瞬态错误自动重试策略
};
```

## 4. OpenAIModel 实现

### 4.1 Format

生成 OpenAI Chat Completion API 格式的请求体：

```json
{
  "model": "gpt-4o",
  "messages": [
    {"role": "system", "content": "..."},
    {"role": "user", "content": "..."},
    {"role": "assistant", "content": "...", "tool_calls": [
      {"id": "call_abc", "type": "function", "function": {"name": "read_file", "arguments": "{\"path\":\"...\"}"}}
    ]},
    {"role": "tool", "tool_call_id": "call_abc", "content": "..."}
  ],
  "tools": [
    {"type": "function", "function": {"name": "read_file", "description": "...", "parameters": {...}}}
  ],
  "stream": true,
  "max_tokens": 4096,
  "temperature": 0.2
}
```

关键处理：
- **原生 function calling**：`tools[]` 数组 + `tool_calls` + `tool_call_id` 协议
- **extraParams**：将 `ConfigNode` 中的扩展参数合并到请求体顶层（如 `max_tokens`、`temperature`）
- **空 tools**：当工具列表为空时不包含 `tools` 字段

### 4.2 Invoke（含自动重试）

`Invoke` 方法内部实现重试循环，不修改 `Model::Invoke` 接口签名。流程：

1. **重试循环**：`attempt` 从 0 到 `1 + maxRetries`
   - 前几次探测尝试传 `nullptr` onChunk（避免碎片内容泄露给前端）
   - 最后一次尝试传真实 onChunk（保留逐字流式体验）
2. **单次调用**：委托 `DoInvokeOnce(formattedInput, onChunk)` 执行 HTTP 请求
   - curl 失败 → 按 `IsRetryableCurlError` 判定 `isRetryable`
   - curl 成功但 HTTP ≥ 400 → 提取状态码，按 `IsRetryableHttpStatus` 判定 `isRetryable`
   - curl 成功且 HTTP < 400 → 正常返回 ModelResponse（`isRetryable = false`）
3. **退避策略**：exponential backoff + jitter
   - `delay = min(baseDelayMs * 2^attempt + jitter, maxDelayMs)`
   - 默认 maxRetries=2, baseDelayMs=400, maxDelayMs=3000 → 最坏额外延迟 ~1.2s
4. **日志**：每次重试记录 `[OpenAI/Anthropic] Retry attempt N/M after Xms (httpCode=... / curlErr=...)`

可重试错误分类：

| 类别 | 可重试 | 示例 |
|------|--------|------|
| HTTP 429 (rate-limit) | ✅ | 请求频率过高 |
| HTTP 5xx (server error) | ✅ | 502/503/504 |
| curl timeout / connection | ✅ | CURLE_OPERATION_TIMEDOUT / COULDNT_CONNECT |
| HTTP 4xx (auth/bad request) | ❌ | 400/401/403 |
| curl init failure | ❌ | 内存不足 |
| JSON parse error | ❌ | 响应格式异常 |

**设计选择**：重试在 Model 实现内部完成而非 `CallModelStream` 层，原因：
1. 不修改 `Model::Invoke` 接口签名，自定义 Model 适配成本不变
2. Model 实现最了解自身错误类型（HTTP 状态码 / curl 错误码）
3. 保持 `CallModelStream` → `ReactWorker` 的交互契约不变

### 4.3 流式解析细节

OpenAI 流式响应中的 tool_calls 是增量式的：

```json
// 第一个 delta
{"delta": {"tool_calls": [{"index": 0, "id": "call_abc", "type": "function", "function": {"name": "read_file", "arguments": ""}}]}}
// 后续 delta
{"delta": {"tool_calls": [{"index": 0, "function": {"arguments": "{\"pa"}}]}}
{"delta": {"tool_calls": [{"index": 0, "function": {"arguments": "th\":\""}}]}}
{"delta": {"tool_calls": [{"index": 0, "function": {"arguments": "/tmp/a\"}"}}]}}
```

`OpenAIModel` 按 `index` 聚合增量片段，最终组装完整的 `ToolCall`。

## 5. AnthropicModel 实现

### 5.1 Format

生成 Anthropic Messages API 格式的请求体：

```json
{
  "model": "claude-3-opus",
  "system": "...",
  "messages": [
    {"role": "user", "content": "..."},
    {"role": "assistant", "content": [
      {"type": "text", "text": "..."},
      {"type": "tool_use", "id": "toolu_abc", "name": "read_file", "input": {"path": "..."}}
    ]},
    {"role": "user", "content": [
      {"type": "tool_result", "tool_use_id": "toolu_abc", "content": "..."}
    ]}
  ],
  "tools": [
    {"name": "read_file", "description": "...", "input_schema": {...}}
  ],
  "stream": true,
  "max_tokens": 4096
}
```

关键差异与 OpenAI 格式：

| 方面 | OpenAI | Anthropic |
|------|--------|-----------|
| 系统提示 | `messages[0] = {"role":"system"}` | 顶层 `system` 字段 |
| 工具调用 | `tool_calls[]` + `tool_call_id` | `tool_use` content block + `tool_use_id` |
| 工具结果 | `{"role":"tool", ...}` | `{"role":"user", "content":[{"type":"tool_result",...}]}` |
| 工具 Schema | `{"type":"function","function":{...}}` | `{"name":..., "input_schema":{...}}` |
| 调用标识 | `call_xxx` | `toolu_xxx` |

### 5.2 Invoke（含自动重试）

与 OpenAI 对称：`Invoke` 内部重试循环 + `DoInvokeOnce` 单次调用。唯一差异是请求端点（`${baseUrl}/v1/messages`）和 SSE 解析格式。重试逻辑完全复用 `retry_helper.h` 的共享函数。

### 5.3 特殊处理

- Anthropic 不支持 `role: system` 在 messages 中，系统提示必须在顶层
- 工具结果以 `user` 消息的 `tool_result` content block 形式发送
- 连续的 `assistant` + `user` 角色必须交替，不允许连续两个相同角色

## 6. Provider / formatType 双路由

### 6.1 设计动机

有些 OpenAI 兼容的 API 在细节上与标准 OpenAI 有差异（如不支持 `role: tool`、不同的流式格式等）。`provider` 字段允许为这些厂商注册专门的处理逻辑。

### 6.2 路由优先级

```
CreateModel(config)
  │  1. 若 config.provider 非空 → 查找 provider 注册
  │  2. 若未找到 → 回退到 formatType 注册
  │  3. 若 config.provider 为空 → 直接使用 formatType 注册
```

示例：

```cpp
// 注册自定义 provider
rm.RegisterModel("volcengine", [](const ModelConfig& cfg) {
    return make_unique<VolcEngineModel>(cfg);  // 处理火山引擎的协议差异
});

// 配置指定 provider
config.modelConfig.formatType = ModelFormatType::OPENAI;
config.modelConfig.provider = "volcengine";
```

## 7. 提示词解析回退模式

### 7.1 设计意图

当 LLM 不支持原生 function calling（或 `useNativeFunctionCalling = false`）时，框架通过提示词引导模型以文本形式输出工具调用，再由 `tool_parser` 解析。

### 7.2 回退模式流程

```
ReactAgentWorker::ReactLoop (回退模式)
  │  1. BuildPrompt 中注入工具调用格式说明
  │  2. Model::Format 不包含 tools[] 字段
  │  3. Model::Invoke 返回纯文本响应
  │  4. ExtractAllToolCalls(content) 解析工具调用
  │     ├── 匹配 [ACTION]{"name":"...", "arguments":{...}} 格式
  │     ├── 匹配 Action: name\nArguments: {...} 格式
  │     └── 为每个解析出的调用生成本地 tool_call_id
  │  5. 若解析出工具调用 → 执行工具，继续循环
  │  6. 若无工具调用 → 作为最终回答
```

### 7.3 tool_parser

`src/utils/tool_parser.h` 提供 `ExtractAllToolCalls()` 函数，支持两种格式：

- `[ACTION]{"name":"read_file","arguments":{"path":"/tmp/a"}}` — JSON 格式
- `Action: read_file\nArguments: {"path":"/tmp/a"}` — 文本格式

解析结果为 `ParsedToolCall` 结构（`name` + `arguments`），与原生 `ToolCall` 不同但语义一致。

## 8. 流式回调设计

### 8.1 onChunk 回调

`Model::Invoke` 的 `onChunk` 参数接收流式文本增量。在框架内部，这个回调最终连接到 `SessionManager::Invoke` 传入的应用层回调。

回调链：

```
应用层 callback
  │  └── SessionManager::Invoke 传入
  │      └── Agent::Invoke 传入
  │          └── AgentWorker::Invoke 传入
  │              └── CallModelStream(systemPrompt, messages, onChunk, generation)
  │                  └── Model::Invoke(formattedInput, onChunk)
  │                      └── curl write callback → onChunk(token)
```

### 8.2 标记注入

框架在流式回调中注入特殊标记：

- `[STATUS]` 标记：工具调用开始/结束、思考状态等
- `[TOOL_CALLS]` 标记：工具调用元数据
- `[TOOL_RESPONSE]` 标记：工具执行结果
- `[FINAL]` 标记：最终回答
- `[ASK_USER]` 标记：请求用户澄清

这些标记由 `ReactAgentWorker` 在执行循环中注入，应用层通过 `AgentResponseHandler` 解析。

## 9. 配置参数

### 9.1 ModelConfig 完整字段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `baseUrl` | string | - | API 端点地址 |
| `apiKey` | string | - | API 认证密钥 |
| `modelName` | string | - | 模型名称 |
| `provider` | string | "" | 自定义提供商标识 |
| `formatType` | ModelFormatType | OPENAI | API 协议格式 |
| `useNativeFunctionCalling` | bool | true | 是否使用原生 function calling |
| `extraParams` | ConfigNode | - | 扩展参数 |
| `retryPolicy.maxRetries` | int | 2 | 瞬态错误重试次数 |
| `retryPolicy.baseDelayMs` | int | 400 | exponential backoff 基础延迟（ms） |
| `retryPolicy.maxDelayMs` | int | 3000 | 退避上限（ms） |
| `retryPolicy.withJitter` | bool | true | 随机抖动避免 thundering herd |

### 9.2 常用 extraParams

```cpp
config.modelConfig.extraParams.Set("max_tokens", 4096);
config.modelConfig.extraParams.Set("temperature", 0.2f);
config.modelConfig.extraParams.Set("top_p", 0.9f);
config.modelConfig.extraParams.Set("presence_penalty", 0.0f);
config.modelConfig.extraParams.Set("frequency_penalty", 0.0f);
config.modelConfig.extraParams.Set("seed", 42);
```

这些参数在 `Format()` 时被合并到请求体的顶层字段中。

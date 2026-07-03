# Worker 执行引擎设计文档

## 1. 模块概述

Worker 执行引擎模块（`src/workers/`、`src/core/agent_worker.h`）实现了 Agent 的核心推理-行动循环。当前仅有 `ReactAgentWorker` 一种实现，采用 ReAct（Reasoning + Acting）模式。`AgentWorker` 是抽象基类，预留了 `PLAN_AND_EXECUTE` 和 `WORKFLOW` 两种工作模式的扩展接口。

模块包含以下组件：

| 类 | 文件 | 职责 |
|----|------|------|
| `AgentWorker` | `src/core/agent_worker.h` | 执行循环抽象基类 |
| `ReactAgentWorker` | `src/workers/react_worker.h` | ReAct 循环实现 |
| `CreateAgentWorker` | `src/workers/agent_worker_factory.cpp` | 工厂函数 |

## 2. AgentWorker 抽象基类

### 2.1 设计意图

`AgentWorker` 定义了执行循环的公共基础设施，包括模型调用、工具执行、提示词组装和取消机制。具体的工作模式（如 ReAct、Plan-and-Execute）实现各自的 `Invoke` 方法。

### 2.2 类结构

```
AgentWorker
  │  ├── config_ (AgentConfig)
  │  ├── toolNames_ (vector<string>)    ← 注册的工具名称列表（唯一拥有）
  │  ├── ownedMcpTools_ (vector<string>) ← 本 worker 加过的 MCP 工具（SyncMcpTools diff 用）
  │  ├── toolSelector_ (unique_ptr<ToolSelector>) ← 工具选择器
  │  ├── skillEngine_ (shared_ptr<SkillEngine>) ← 技能引擎
  │  ├── workerEnv_ (WorkerEnv*)        ← 环境接口（非拥有，由 SessionManager 注入）
  │  ├── cancelGeneration_ (atomic<uint64_t>) ← 取消代数计数器
  │  ├── toolMutex_ (mutex)             ← 保护 toolNames_ + ownedMcpTools_ + toolSelector_
  │  │
  │  ├── 纯虚方法:
  │  │   Invoke(query, contextEngine, callback)
  │  │
  │  ├── 保护方法:
  │  │   CallModelStream(systemPrompt, messages, onChunk, generation)
  │  │   BuildToolSchemas()
  │  │   BuildPrompt(templateName, query, context, contextEngine)
  │  │   ExecuteTool(toolName, input, streamCallback)
  │  │   GetToolSchemaForQuery(query)
  │  │   GetTodoSnippet()
  │  │   IsCancelled(myGeneration)
  │  │   CurrentCancelGeneration()
```

> **工具状态单一所有权**：`AgentWorker` 是工具状态的**唯一拥有者**——`toolNames_`（注册工具名）、`ownedMcpTools_`（MCP 归属 diff）、`toolSelector_`（选择器）均在 `toolMutex_`（L5）下。`Agent` 不持工具状态副本，`Agent::AddTools` / `SyncMcpTools` / `GetRegisteredTools` 是纯代理，直接转发到 worker。工具管理设施放 `AgentWorker` 基类，3 种 worker 子类（React / Plan&Execute / Workflow）继承共享、只重写循环。`AddTools` / `SyncMcpTools` 对 `ResourceManager` 的访问（`HasTool` / `GetMcpToolNames`）在 `toolMutex_` **外**完成（避免与 `ResourceManager::toolMutex_` 的 L5→L5 嵌套，见锁排序协议约束 2）。

### 2.3 模板方法模式

`AgentWorker` 使用**模板方法模式**：
- 基类提供基础设施方法（`CallModelStream`、`ExecuteTool`、`BuildPrompt` 等）
- 子类实现具体的执行循环策略（`Invoke`）

子类可以自由组合基类方法，形成不同的执行策略。

### 2.4 取消机制

```cpp
void Cancel();
bool IsCancelled(uint64_t myGeneration) const;
uint64_t CurrentCancelGeneration();
```

取消机制基于**代数计数器（generation counter）**：

```
Cancel()
  │  cancelGeneration_ += 100
  │  ← 递增 100，使所有正在运行的 invocation 失效

CurrentCancelGeneration()
  │  return cancelGeneration_.load()
  │  ← 返回当前值（不递增），作为本次 invocation 的基线快照

IsCancelled(myGeneration)
  │  return cancelGeneration_.load() > myGeneration
  │  ← 名实相符：已取消返回 true
```

**为什么递增 100？**

- 一次 `Cancel` 使所有当前 invocation 失效
- 递增 100 确保即使有多次 `CurrentCancelGeneration`（每次返回同一值），也能正确失效
- 不同会话共享同一 worker，但各自持有不同的 generation 值
- 一次会话的取消不会影响其他正在进行的会话（它们的 generation 值不同）

**使用方式**：

```
ReactAgentWorker::Invoke
  │  myGeneration = CurrentCancelGeneration()
  │  ReactLoop(query, contextEngine, callback, myGeneration)
  │    │  每次迭代检查:
  │    │  ├── IsCancelled(myGeneration) 为真 → 中止循环
  │    │  └── CallModelStream 中检查取消
  │    │      ├── 若已取消 → 停止处理流式数据
  │    │      └── 返回 cancelled ModelResponse
```

## 3. ReactAgentWorker ReAct 循环

### 3.1 设计意图

`ReactAgentWorker` 实现了标准的 ReAct（Reasoning + Acting）模式：模型先推理（Thought），再行动（Action），然后观察结果（Observation），循环进行直到得出最终回答。

### 3.2 类定义

```cpp
class ReactAgentWorker : public AgentWorker {
public:
    ReactAgentWorker(AgentConfig config);
    std::string Invoke(const std::string& query, ContextEngine* contextEngine,
                       std::function<void(const std::string&)> callback) override;
private:
    std::string ReactLoop(const std::string& query, ContextEngine* contextEngine,
                          std::function<void(const std::string&)> callback,
                          uint64_t myGeneration);
};
```

### 3.3 Invoke 流程

```
ReactAgentWorker::Invoke(query, contextEngine, callback)
  │  1. myGeneration = CurrentCancelGeneration()
  │  2. 返回 ReactLoop(query, contextEngine, callback, myGeneration)
```

### 3.4 ReactLoop 完整流程

```
ReactLoop(query, contextEngine, callback, myGeneration)
  │
  │  iterations = 0
  │  while (iterations < config_.maxIterations && !IsCancelled(myGeneration))
  │  │
  │  │  ┌─ Step 1: 组装系统提示 ──────────────────────────────────┐
  │  │  │  systemPrompt = BuildPrompt("react_system", query,      │
  │  │  │      context, contextEngine)                              │
  │  │  │  ├── 从 config_.promptTemplates["react_system"] 加载模板│
  │  │  │  ├── 渲染占位符:                                         │
  │  │  │  │   {$query} → query                                    │
  │  │  │  │   {$context} → contextEngine->GetContextAsString()    │
  │  │  │  │   {$memory} → LoadMemoryContent()                     │
  │  │  │  │   {$skills} → skillEngine_->GetSkillCatalog()         │
  │  │  │  │   {$todo} → GetTodoSnippet()                          │
  │  │  │  └── 返回完整系统提示                                    │
  │  │  └───────────────────────────────────────────────────────────┘
  │  │
  │  │  ┌─ Step 2: 获取上下文窗口 ─────────────────────────────────┐
  │  │  │  messages = contextEngine->GetContextWindow()              │
  │  │  │  ├── ApplyContextLimits → 消息分段 + 压缩               │
  │  │  │  ├── 清理孤立消息                                       │
  │  │  │  └── 返回裁剪后的消息列表                               │
  │  │  │  ※ 每轮迭代都重新获取（而非循环外取一次后本地累加）。   │
  │  │  │    上一轮的 assistant/tool 已通过 AddMessage 写入引擎，   │
  │  │  │    故此处天然包含它们且已受 maxContextTokens/maxMessages  │
  │  │  │    限制——发送给模型的历史不会无界增长。最近一轮用户      │
  │  │  │    query 位于最新分段起点，裁剪/压缩时被优先保留。        │
  │  │  └───────────────────────────────────────────────────────────┘
  │  │
  │  │  ┌─ Step 3: 构建工具 Schema ─────────────────────────────────┐
  │  │  │  toolSchemas = BuildToolSchemas()                         │
  │  │  │  ├── 对 toolNames_ 中每个工具名:                         │
  │  │  │  │   ├── 无状态 → CreateTool → GetJsonSchema             │
  │  │  │  │   ├── 会话级 → CreateSessionTool(ctx) → GetJsonSchema │
  │  │  │  │   ├── MCP → 从 MCPConnection 获取                    │
  │  │  │  │   └── 未知 → 跳过                                    │
  │  │  │  └── 返回 vector<ToolSchema>                             │
  │  │  └───────────────────────────────────────────────────────────┘
  │  │
  │  │  ┌─ Step 4: 调用模型 ─────────────────────────────────────────┐
  │  │  │  response = CallModelStream(systemPrompt,                 │
  │  │  │      messages, callback, myGeneration)                    │
  │  │  │  ├── Model::Format(systemPrompt, messages, toolSchemas)   │
  │  │  │  ├── Model::Invoke(formattedBody, onChunk)                │
  │  │  │  ├── 解析流式响应:                                       │
  │  │  │  │   ├── 文本增量 → onChunk → callback                   │
  │  │  │  │   └── tool_calls → 缓存到 response                    │
  │  │  │  └── 返回 ModelResponse                                  │
  │  │  └───────────────────────────────────────────────────────────┘
  │  │
  │  │  ┌─ Step 5: 判断响应类型 ─────────────────────────────────────┐
  │  │  │  若 config_.modelConfig.useNativeFunctionCalling == true: │
  │  │  │  ├── response.toolCalls 非空 → 有工具调用                │
  │  │  │  └── response.toolCalls 空 → 最终回答                    │
  │  │  │                                                          │
  │  │  │  若 config_.modelConfig.useNativeFunctionCalling == false:│
  │  │  │  ├── ExtractAllToolCalls(response.content) → 解析工具调用│
  │  │  │  ├── 解析出工具 → 有工具调用                             │
  │  │  │  └── 无解析 → 最终回答                                   │
  │  │  └───────────────────────────────────────────────────────────┘
  │  │
  │  │  ┌─ Step 6: 处理最终回答 ─────────────────────────────────────┐
  │  │  │  若无工具调用:                                           │
  │  │  │  ├── contextEngine->AddMessage(assistantMsg)              │
  │  │  │  ├── callback("[FINAL]" + content + "[/FINAL]")          │
  │  │  │  └── 返回 content                                        │
  │  │  └───────────────────────────────────────────────────────────┘
  │  │
  │  │  ┌─ Step 7: 处理工具调用 ─────────────────────────────────────┐
  │  │  │  对每个 ToolCall:                                        │
  │  │  │  ├── callback("[TOOL_CALLS]" + 格式化工具调用信息)        │
  │  │  │  ├── contextEngine->AddMessage(assistant toolCallsMsg)     │
  │  │  │  │   ← 保存 assistant 消息（含 tool_calls）              │
  │  │  │  │                                                      │
  │  │  │  ├── ExecuteTool(toolCall.name, toolCall.argumentsJson,   │
  │  │  │  │              callback)                                │
  │  │  │  │   ├── 构建 ToolBuildContext                           │
  │  │  │  │   ├── 判断工具类型 → 创建实例                         │
  │  │  │  │   ├── tool->Invoke(input) → result                    │
  │  │  │  │   └── 返回 result                                     │
  │  │  │  │                                                      │
  │  │  │  ├── Payload Offloading (可选):                          │
  │  │  │  │   若 result.length() >= offloadToolResultChars        │
  │  │  │  │   && memoryRuntime_ 存在 && enablePayloadOffload:     │
  │  │  │  │   ├── WritePayload(request) → offloadResult          │
  │  │  │  │   ├── 若 offloadResult.offloaded:                    │
  │  │  │  │   │   ├── result = offloadResult.replacementContent  │
  │  │  │  │   │   └── payloadRef = offloadResult.payload.uri     │
  │  │  │  │   └── 若 offload 失败: result 不变                   │
  │  │  │  │                                                      │
  │  │  │  ├── callback("[TOOL_RESPONSE id]" + 摘要 + "[/TOOL_RESPONSE]")
  │  │  │  ├── contextEngine->AddMessage(toolResultMsg)
  │  │  │  │   ← role="tool", toolCallId, content=result, payloadRef
  │  │  │  └── 继续下一个 ToolCall                                 │
  │  │  └───────────────────────────────────────────────────────────┘
  │  │
  │  │  iterations++
  │  │  继续循环
  │  │
  │  └── 循环结束 → 返回最后一条 assistant 消息的 content
```

## 4. 工具调用双路径

### 4.1 原生 function calling 路径

当 `ModelConfig::useNativeFunctionCalling = true` 时：

```
Model::Format 包含 tools[] 字段
  │  → 模型返回结构化的 tool_calls (OpenAI: tool_calls[]; Anthropic: tool_use blocks)
  │  → ModelResponse.toolCalls 已被解析
  │  → 直接使用，无需文本解析
```

优势：
- 模型准确生成工具调用（有 Schema 指导）
- 参数格式标准（JSON）
- 工具调用 ID 由 API 返回，确保消息链完整
- 减少解析错误

### 4.2 提示词回退路径

当 `ModelConfig::useNativeFunctionCalling = false` 时：

```
Model::Format 不包含 tools[] 字段
  │  → 系统提示中包含工具调用格式说明
  │  → 模型以文本形式输出工具调用
  │  → ExtractAllToolCalls(response.content) 解析
  │  → 为每个解析出的调用生成本地 tool_call_id
```

支持的格式：
- `[ACTION]{"name":"read_file","arguments":{"path":"/tmp/a"}}`
- `Action: read_file\nArguments: {"path":"/tmp/a"}`

回退路径适用于：
- 不支持原生 function calling 的模型
- 定制化工具调用格式的场景
- 测试和调试

### 4.3 路径选择

```
ReactLoop Step 5:
  │  ├── useNativeFunctionCalling == true:
  │  │   ├── response.toolCalls.size() > 0 → 工具调用路径
  │  │   └── response.toolCalls.size() == 0 → 最终回答
  │  │
  │  └── useNativeFunctionCalling == false:
  │  │   ├── ExtractAllToolCalls(response.content) → parsedCalls
  │  │   ├── parsedCalls.size() > 0 → 工具调用路径 (使用 parsedCalls)
  │  │   └── parsedCalls.size() == 0 → 最终回答
```

## 5. CallModelStream 详细设计

### 5.1 函数签名

```cpp
ModelResponse CallModelStream(
    const std::string& systemPrompt,
    const std::vector<Message>& messages,
    std::function<void(const std::string&)> onChunk,
    uint64_t generation = 0);
```

> 注：`shouldCancel` 取消回调不暴露在 `CallModelStream` 签名上——它在内部由 `generation` 派生（`[this,gen]{ return IsCancelled(gen); }`）传给 `Model::Invoke`。详见 5.3。

### 5.2 执行流程

```
CallModelStream(systemPrompt, messages, onChunk, generation)
  │  0. 前置取消门控:
  │     ├── IsCancelled(generation) → 直接返回 ModelResponse{finishReason="cancelled", isFinished=true}
  │     └── 否则继续
  │
  │  1. 创建 Model 实例 (每次调用新建, 生命周期见 5.4):
  │     └── ResourceManager::CreateModel(config_.modelConfig) → unique_ptr<Model>
  │
  │  2. 构建 ToolSchema 列表:
  │     ├── BuildToolSchemas() → vector<ToolSchema>
  │     └── 若 useNativeFunctionCalling → 包含在 Format 中
  │     └── 若 !useNativeFunctionCalling → 不包含（回退模式在提示词中说明）
  │
  │  3. 格式化请求:
  │     ├── model->Format(systemPrompt, messages, toolSchemas)
  │     └── → formattedBody (JSON 字符串)
  │
  │  4. 流式调用 (传 shouldCancel 回调, 见 5.3):
  │     ├── model->Invoke(formattedBody, onChunk, shouldCancel)
  │     │   ├── CurlClient::PostStream 发送 HTTP 请求 (thread_local CURL handle 复用)
  │     │   ├── 接收 SSE 流式响应
  │     │   ├── 文本增量 → onChunk → 外部 callback
  │     │   ├── tool_calls → 缓存到 StreamContext
  │     │   └── 取消检查: shouldCancel() → 中止传输 (见 5.3)
  │     └── → ModelResponse
  │
  │  5. 返回 ModelResponse
  │
  │  注: 步骤 1-4 全部包裹在 try/catch 中, 异常处理见 5.5
```

> 步骤顺序按代码实际：CreateModel → BuildToolSchemas → Format → Invoke（早期文档把 BuildToolSchemas 放在 CreateModel 之前，与代码不符，已更正）。

### 5.3 取消集成

取消在三个层面检查，覆盖"调用前"、"流式中"、"重试间"三种场景：

```
(0) 前置门控 (CallModelStream 入口):
    └── IsCancelled(generation) → 返回 {finishReason="cancelled", isFinished=true}
        （廉价早退，避免无谓的 CreateModel/Format/Invoke）

(1) mid-stream 取消 (流式输出途中, 真正中止 HTTP 传输):
    shouldCancel = [this,generation]{ return IsCancelled(generation); }
    model->Invoke(formatted, onChunk, shouldCancel)
      └── DoInvokeOnce 的 PostStream chunk callback:
          ├── ProcessSSEChunk(ctx, data, len)   // 先解析这批 chunk
          ├── if (ctx.shouldCancel && ctx.shouldCancel()):
          │     ├── ctx.cancelled = true
          │     └── return false                // curl WRITEFUNCTION 返回 0
          │         → curl_easy_perform 返回 CURLE_WRITE_ERROR, 中止传输
          └── else: return true                 // 继续接收
      └── 返回后 DoInvokeOnce 检查 ctx.cancelled:
          └── 返回 {finishReason="cancelled", isFinished=true, isRetryable=false,
                   content=ctx.fullText}        // 已生成的 token 已经过 onChunk 推出

(2) 重试循环边界 (Model::Invoke retry loop):
    每次 attempt 开头检查 shouldCancel() → 返回 cancelled
    （覆盖上一次 attempt 的 backoff 窗口 + 首次调用前）
    若 DoInvokeOnce 返回 finishReason=="cancelled" → 不重试, 直接返回
```

**关键设计**：
- mid-stream 取消是**物理中止**（curl 传输真正停止），非逻辑抑制——通过 PostStream 的 bool 返回 callback 实现（`CurlClient::PostStream` 的 onChunk 返回 false → WRITEFUNCTION 返回 0 → curl 中止）
- 用 `ctx.cancelled` 标志区分"主动中止"（应返回 cancelled）与"真 write 错误"（应返回 error + isRetryable）——两者都会让 curl 返回 `CURLE_WRITE_ERROR`，但只有前者置 `cancelled=true`
- 取消响应 `{finishReason="cancelled", isFinished=true, isRetryable=false}`，上层 ReactLoop 已有 `if (resp.finishReason == "cancelled")` 处理（react_worker.cpp）
- `shouldCancel` 检查在每批 chunk **解析之后**：已收到的 token 先经 onChunk 推出再中止，避免丢失已生成内容

> 注：完全异步化（HTTP 阻塞期间响应取消、退避 sleep 期间可中断）仍是 TODO（P2 Model::Invoke 异步化剩余项）。当前 shouldCancel 只能在 chunk 边界 + attempt 边界检查。

### 5.4 Model 实例生命周期与连接复用

每次 `CallModelStream` 调用都通过 `ResourceManager::CreateModel` 新建一个 `Model` 实例，实例仅在该次调用作用域内存活，函数返回时析构。在 `ReactLoop` 中每个 iteration 调用一次 `CallModelStream`，因此每次迭代都创建一个新的 `Model` 对象。

**为何 per-call 新建是可接受的**：

`Model` 对象本身是**轻量配置持有者**——只持有 `ModelConfig`（几个 string + RetryPolicy），无连接状态、无可变运行时数据。创建/析构开销可忽略。

真正的连接状态（CURL handle、TCP 连接、TLS session、DNS cache）不在 `Model` 对象内，而在 `CurlClient` 的 **thread_local 持久化 handle** 中（`src/utils/curl_client.cpp` 的 `ThreadCurlHandle`）。因此：

- 每次 `CallModelStream` 新建 `Model` 对象 → 不会触发 `curl_easy_init`（仅 `ModelConfig` 拷贝）
- 同一线程内连续的模型调用 → 复用同一个 thread_local CURL handle（`curl_easy_reset` 清选项、保留连接缓存）
- 跨 iteration 的连接复用 → 由 CurlClient 的 thread_local 机制保证，与 `Model` 对象是否缓存无关

**为何不缓存 Model 对象**：`Model` 对象本身是轻量配置持有者，per-call 新建/析构开销可忽略，缓存它没有性能收益；反而会引入生命周期管理、配置热重载刷新、线程安全等额外复杂度。真正的连接复用由 CurlClient 的 thread_local handle 承担，与 `Model` 对象是否缓存无关，因此 per-call 新建 `Model` + thread_local handle 复用即可同时获得简洁性与连接复用收益，无需缓存 `Model` 对象。

### 5.5 异常处理

`CallModelStream` 的步骤 1-4（CreateModel / BuildToolSchemas / Format / Invoke）全部包裹在 `try { ... } catch (const std::exception&) { ... }` 中：

```
try {
    auto model = ResourceManager::CreateModel(...);   // 步骤 1
    auto tools = BuildToolSchemas();                  // 步骤 2
    auto formatted = model->Format(...);              // 步骤 3
    out = model->Invoke(formatted, onChunk, shouldCancel);  // 步骤 4
} catch (const std::exception& e) {
    out.content = "Model Error: " + e.what();
    out.finishReason = "error";
    out.isFinished = true;
    if (onChunk) onChunk(out.content);   // 将错误文本作为 chunk 推给外部 callback
}
return out;  // 步骤 5
```

**语义**：
- **任何异常都转为终止性 ModelResponse**：`finishReason="error"`、`isFinished=true`，避免上层 ReactLoop 在异常态继续迭代
- **错误文本经 onChunk 推送**：使流式调用方（如 HTTP SSE）能在异常时收到错误消息，而非静默失败
- **无"CreateModel 失败 → 返回空 ModelResponse"分支**：`CreateModel` 失败会抛异常，由本 catch 统一处理（早期文档"若失败 → 返回空 ModelResponse"的描述不准确，已更正）
- **Model::Invoke 内部的可重试错误**（HTTP 5xx / curl 临时错误）已在 Invoke 的 retry loop 内消耗并重试，不会冒泡到本 try/catch；只有不可重试或耗尽重试的最终错误才作为正常 `ModelResponse` 返回（非异常）

## 6. BuildToolSchemas 详细设计

### 6.1 函数签名

```cpp
std::vector<ToolSchema> BuildToolSchemas() const;
```

### 6.2 执行流程

```
BuildToolSchemas()
  │  ├── 对 toolNames_ 中每个名称:
  │  │   ├── ResourceManager::HasTool(name) → 无状态工具
  │  │   │   ├── ResourceManager::CreateTool(name) → 临时实例
  │  │   │   ├── tool->GetJsonSchema() → ToolSchema
  │  │   │   └── 释放临时实例
  │  │   │
  │  │   ├── ResourceManager::HasSessionTool(name) → 会话级工具
  │  │   │   ├── 构建 ToolBuildContext (使用 WorkerEnv 获取依赖)
  │  │   │   ├── ResourceManager::CreateSessionTool(name, ctx) → 临时实例
  │  │   │   ├── tool->GetJsonSchema() → ToolSchema
  │  │   │   └── 释放临时实例
  │  │   │
  │  │   ├── ResourceManager::HasMCPServer(serverName) → MCP 工具
  │  │   │   ├── 从 MCPConnection 获取 MCPToolInfo
  │  │   │   └── 转换为 ToolSchema
  │  │   │
  │  │   └── 未知 → 跳过
  │  │
  │  └── 返回 vector<ToolSchema>
```

注意：Schema 构建创建临时工具实例仅用于获取 Schema。会话级工具的 Schema 与具体 ToolBuildContext 无关（Schema 描述参数结构，不涉及运行时依赖）。

## 7. BuildPrompt 详细设计

### 7.1 函数签名

```cpp
std::string BuildPrompt(const std::string& templateName, const std::string& query,
                         const std::string& context, ContextEngine* contextEngine);
```

### 7.2 执行流程

```
BuildPrompt(templateName, query, context, contextEngine)
  │  1. 查找模板:
  │     ├── config_.promptTemplates[templateName]
  │     └── PromptResource{type, value}
  │
  │  2. 加载模板内容:
  │     ├── type == TEXT → 直接使用 value
  │     ├── type == FILE_PATH → ResolvePromptResource(resource)
  │     │   ├── 读取文件内容
  │     │   └── 返回文本
  │
  │  3. 渲染占位符 (RenderPrompt):
  │     ├── {$query} → query
  │     ├── {$context} → context (来自 ContextEngine)
  │     ├── {$memory} → LoadMemoryContent()
  │     │   ├── 若 WorkerEnv::GetMemoryRuntime() 存在:
  │     │   │   → memoryRuntime->BuildContext(request).memoryText
  │     │   └── 否则 → 空字符串
  │     ├── {$skills} → skillEngine_->GetSkillCatalog()
  │     └── {$todo} → GetTodoSnippet()
  │
  │  4. 返回渲染后的提示文本
```

### 7.3 prompt_utils 工具函数

`src/utils/prompt_utils.h` 提供：

- `ResolvePromptResource(resource)` — 加载 PromptResource（文件路径或内联文本）
- `RenderPrompt(template, replacements)` — 替换 `{$KEY}` 占位符

## 8. ExecuteTool 详细设计

### 8.1 函数签名

```cpp
std::string ExecuteTool(const std::string& toolName, const std::string& input,
                         const std::function<void(const std::string&)>& streamCallback);
```

### 8.2 执行流程

```
ExecuteTool(toolName, input, streamCallback)
  │  1. 判断工具类型:
  │     ├── ResourceManager::HasSessionTool(toolName) → 会话级
  │     │   ├── 构建 ToolBuildContext:
  │     │   │   ├── todoList = workerEnv_->GetOrCreateSessionTodoList(sessionId)
  │     │   │   ├── askUser = workerEnv_->GetAskUserDispatcher(sessionId)
  │     │   │   ├── memoryRuntime = workerEnv_->GetMemoryRuntime()
  │     │   │   ├── streamCallback = 当前的流式回调
  │     │   │   └── sessionId = 当前会话 ID
  │     │   ├── ResourceManager::CreateSessionTool(toolName, ctx)
  │     │   └── tool->Invoke(input)
  │     │
  │     ├── ResourceManager::HasTool(toolName) → 无状态
  │     │   ├── ResourceManager::CreateTool(toolName)
  │     │   └── tool->Invoke(input)
  │     │
  │     ├── ResourceManager::HasMCPServer → MCP 工具
  │     │   ├── ResourceManager::CreateTool(toolName) → MCPTool 代理
  │     │   └── tool->Invoke(input)
  │     │
  │     └── 未知工具 → 返回错误消息
  │
  │  2. 返回工具结果字符串
```

## 9. AgentWorkerFactory 工厂函数

### 9.1 函数定义

```cpp
std::unique_ptr<AgentWorker> CreateAgentWorker(AgentConfig config);
```

### 9.2 路由逻辑

```
CreateAgentWorker(config)
  │  ├── config.mode == REACT → make_unique<ReactAgentWorker>(config)
  │  ├── config.mode == PLAN_AND_EXECUTE → 未实现，throw invalid_argument
  │  ├── config.mode == WORKFLOW → 未实现，throw invalid_argument
  │  └── 默认 → throw invalid_argument (unknown mode)
```

`PLAN_AND_EXECUTE` 和 `WORKFLOW` 模式预留但未实现，是未来扩展的方向。

## 10. 迭代限制与安全保护

### 10.1 maxIterations

`AgentConfig::maxIterations` 限制 ReAct 循环的最大迭代数，防止：
- 模型陷入无限循环（反复调用同一工具）
- 模型无法生成最终回答
- 资源耗尽

默认值：10 次。超出后，返回最后一次模型输出的内容。

### 10.2 取消安全

- `Cancel()` 递增 `cancelGeneration_` 100，确保所有 invocation 失效
- 流式回调中每 chunk 检查取消状态
- 工具执行结果仍被添加到 ContextEngine（即使已取消），确保上下文完整性

### 10.3 错误处理

- 工具执行失败 → 结果为错误消息字符串，继续循环
- 模型调用失败 → 返回错误消息
- 未知工具 → 返回错误消息，模型可选择其他工具

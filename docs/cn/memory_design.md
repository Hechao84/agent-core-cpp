# 记忆系统设计文档

## 1. 模块概述

记忆系统是 jiuwen-lite 的核心差异化能力，为 Agent 提供跨会话的长期记忆、上下文增强和智能整合。记忆系统采用**可插拔接口**设计，支持进程内内置运行时和 HTTP 远程运行时两种模式，同时预留动态插件扩展能力。

记忆系统包含以下组件：

| 类/接口 | 文件 | 职责 |
|---------|------|------|
| `MemoryRuntime` | `include/memory_runtime.h` | 记忆运行时抽象接口 |
| `MemoryConfig` | `include/memory_config.h` | 记忆运行时配置 |
| `MemoryTypes` | `include/memory_types.h` | 记忆数据类型定义 |
| `BuiltinMemoryRuntime` | `src/memory/builtin_memory_runtime.h` | 内置进程内实现 |
| `HttpMemoryRuntime` | `src/memory/http_memory_runtime.h` | HTTP 远程代理实现 |
| `type_bridge` | `src/memory/type_bridge.h` | 类型双向转换桥 |
| `LongTermConsolidator` | `src/memory/long_term_consolidator.h` | 整合抽象接口 |
| `LegacyDreamConsolidator` | `src/memory/long_term_consolidator.h` | 遗留 Dream 整合实现 |

## 2. MemoryRuntime 抽象接口

### 2.1 七个核心方法

```cpp
class MemoryRuntime {
public:
    explicit MemoryRuntime(MemoryConfig config);
    virtual ~MemoryRuntime() = default;

    virtual bool AppendEvent(const MemoryEvent& event) = 0;
    virtual MemoryContextPackage BuildContext(const MemoryContextRequest& request) = 0;
    virtual MemoryPayloadWriteResult WritePayload(const MemoryPayloadWriteRequest& request) = 0;
    virtual std::string ReadPayload(const std::string& uri) = 0;
    virtual bool Consolidate(const MemoryConsolidationRequest& request) = 0;
    virtual bool Consolidate(const MemoryConsolidationRequest& request,
                             MemoryModelClient* modelClient) = 0;
    virtual std::vector<MemorySearchHit> SearchMemory(const MemorySearchRequest& request) = 0;
    virtual MemoryStats GetStats() const = 0;
};
```

### 2.2 方法职责

| 方法 | 调用时机 | 职责 |
|------|---------|------|
| `AppendEvent` | ContextEngine::AddMessage | 将对话事件追加到短期记忆流 |
| `BuildContext` | AgentWorker::BuildPrompt | 为当前查询构建长期记忆上下文 |
| `WritePayload` | ReactWorker (工具结果 offload) | 将大型工具结果写入外部存储 |
| `ReadPayload` | MemoryReadPayloadTool | 按需读取 offloaded payload |
| `Consolidate` | Agent::ConsolidationLoop | 将短期事件整合为长期记忆。双重载：无参版让 runtime 自决模型来源；带参版用显式提供的 modelClient（对 HTTP 模式无效，server 用自己的模型） |
| `SearchMemory` | 搜索查询 | 搜索长期记忆中的实体和关系 |
| `GetStats` | 管理界面 | 获取运行时统计信息 |

### 2.3 设计原则

- **接口隔离**：`MemoryRuntime` 不依赖框架的 `Model` 类。LLM 调用通过 `MemoryModelClient` 接口注入，插件只依赖此轻量接口。
- **配置驱动**：所有实现通过 `MemoryConfig` 初始化，包含 mode、provider、dataPath 等。
- **所有权设计**：`MemoryRuntime` 由 `SessionManager` 拥有（`unique_ptr`），不随 Agent 热重载销毁，确保 `ContextEngine` 回调的一致性。

## 3. 核心数据类型

### 3.1 MemoryEvent

```cpp
struct MemoryEvent {
    MemoryEventType type;    // 事件类型枚举
    std::string agentId;
    std::string sessionId;
    std::string role;
    std::string content;
    std::string toolCallId;
    std::string toolName;
    std::string payloadRef;
    std::string storeCursor;
    nlohmann::json metadata;
    std::string timestamp;
};
```

事件类型枚举：

| 类型 | 说明 |
|------|------|
| `SESSION_STARTED` | 会话开始 |
| `SESSION_ENDED` | 会话结束 |
| `MESSAGE_APPENDED` | 新消息追加 |
| `TOOL_CALL_STARTED` | 工具调用开始 |
| `TOOL_CALL_FINISHED` | 工具调用完成 |
| `PAYLOAD_OFFLOADED` | Payload 被 offload |
| `CONSOLIDATION_REQUESTED` | 整合请求 |
| `CONSOLIDATION_COMPLETED` | 整合完成 |

### 3.2 MemoryEntity 与 MemoryRelation

```cpp
struct MemoryEntity {
    std::string id;
    std::string agentId;
    std::string entityType;  // "person", "project", "concept" 等
    std::string name;
    std::string summary;
    float confidence;
    bool isActive;
    std::string supersededByEntityId;   // 被替代的新实体 ID
    std::string supersededEntityId;     // 被此实体替代的旧实体 ID
    std::vector<std::string> sourceRefs;
    nlohmann::json metadata;
    std::string createdAt / updatedAt;
};

struct MemoryRelation {
    std::string id;
    std::string agentId;
    std::string fromEntityId;
    std::string relationType;  // "works_for", "depends_on" 等
    std::string toEntityId;
    float confidence;
    std::vector<std::string> sourceRefs;
    nlohmann::json metadata;
    std::string createdAt / updatedAt;
};
```

实体-关系模型支持：
- 知识图谱式的长期记忆
- 实体替代（`supersededByEntityId`）追踪知识演化
- 来源引用（`sourceRefs`）确保可追溯性
- 置信度评分

### 3.3 MemoryPayloadRef

```cpp
struct MemoryPayloadRef {
    std::string agentId;
    std::string sessionId;
    std::string uri;           // payload 文件的 URI
    std::string contentType;
    std::string summary;       // 简短摘要（替代上下文中的完整内容）
    std::string toolName;
    int originalChars;         // 原始内容的字符数
    nlohmann::json metadata;
    std::string createdAt;
};
```

### 3.4 MemoryContextPackage

```cpp
struct MemoryContextPackage {
    std::vector<MemoryMessage> messages;    // 相关的历史消息片段
    std::string memoryText;                 // 长期记忆文本摘要
    std::vector<MemoryEntity> entities;     // 相关实体
    std::vector<MemoryRelation> relations;  // 相关关系
    std::vector<MemoryPayloadRef> payloadRefs;  // 可引用的 payload
    std::vector<std::string> citations;     // 来源引用
    nlohmann::json metadata;
};
```

`memoryText` 是注入系统提示的核心字段，包含长期记忆的文本摘要。

### 3.5 MemoryModelClient

```cpp
class MemoryModelClient {
public:
    virtual ~MemoryModelClient() = default;
    virtual MemoryModelResult GenerateMemoryUpdate(const std::string& prompt) = 0;
};
```

这是记忆运行时与 LLM 交互的**最小接口**。插件只依赖此接口，不依赖框架的 `Model` 类，降低耦合。

`HostMemoryModelClient`（在 `agent.cpp` 中实现）适配框架 `Model` 到此接口：

```cpp
class HostMemoryModelClient : public MemoryModelClient {
    Model* model_;
public:
    MemoryModelResult GenerateMemoryUpdate(const std::string& prompt) override {
        // 使用框架的 Model 执行 LLM 调用
        // 将结果转换为 MemoryModelResult
    }
};
```

## 4. BuiltinMemoryRuntime

### 4.1 设计意图

`BuiltinMemoryRuntime` 是进程内的记忆运行时实现，桥接到 `agent-memory-cpp` 子库。它仅在 `JIUWEN_ENABLE_MEMORY_BUILTIN` 编译条件下可用。

### 4.2 桥接模式

```
BuiltinMemoryRuntime
  │  ├── impl_ (unique_ptr<agent_memory::BuiltinMemoryRuntime>)
  │  │   ← agent-memory-cpp 的进程内实现
  │  │
  │  ├── AppendEvent(event)
  │  │   ├── ToAgentEvent(event) → agent_memory::MemoryEvent
  │  │   └── impl_->AppendEvent(agentEvent)
  │  │
  │  ├── BuildContext(request)
  │  │   ├── ToAgentContextRequest(request) → agent_memory::MemoryContextRequest
  │  │   ├── impl_->BuildContext(agentRequest) → agent_memory::MemoryContextPackage
  │  │   └── FromAgentContextPackage(agentPkg) → jiuwen::MemoryContextPackage
  │  │
  │  ├── WritePayload / ReadPayload / Consolidate / SearchMemory / GetStats
  │  │   └── 同样的双向类型转换模式
```

### 4.3 type_bridge 双向转换

`type_bridge.h` 提供 `jiuwen` 公共类型与 `agent_memory` 内部类型的双向转换：

```cpp
// jiuwen → agent_memory
agent_memory::MemoryConfig ToAgentMemoryConfig(const MemoryConfig& cfg);
agent_memory::MemoryEvent ToAgentEvent(const MemoryEvent& event);
agent_memory::MemoryPayloadWriteRequest ToAgentPayloadWriteRequest(const MemoryPayloadWriteRequest& req);
// ...

// agent_memory → jiuwen
MemoryContextPackage FromAgentContextPackage(const agent_memory::MemoryContextPackage& pkg);
MemoryPayloadWriteResult FromAgentPayloadWriteResult(const agent_memory::MemoryPayloadWriteResult& result);
MemorySearchHit FromAgentSearchHit(const agent_memory::MemorySearchHit& hit);
// ...
```

这种**Bridge 模式**确保：
- 框架其余部分完全不受 `agent_memory::` 依赖影响
- `agent-memory-cpp` 可独立演进，不影响公共 API
- `BuiltinMemoryRuntime` 是唯一依赖 `agent-memory-cpp` 的编译单元

### 4.4 存储布局

```
data/memory_runtime/
 ├── memory.db              # agent-memory-cpp SQLite 数据库
 │                          # 包含：events 表、entities 表、relations 表、payload_refs 表
 └── payloads/              # offloaded payload 文件
     └── *.txt              # 每个文件对应一个 payloadRef.uri
```

## 5. HttpMemoryRuntime

### 5.1 设计意图

`HttpMemoryRuntime` 是 HTTP 远程代理实现，将所有 `MemoryRuntime` 操作转发到外部记忆服务器。适用于：
- 记忆服务独立部署（微服务架构）
- 多 Agent 共享同一记忆服务
- 不需要 `agent-memory-cpp` 编译依赖的场景

### 5.2 代理模式

```
HttpMemoryRuntime
  │  ├── serverUrl_ (string)       ← 记忆服务器 URL
  │  ├── apiKey_ (string)          ← 认证密钥
  │  ├── timeoutSeconds_ (int)     ← HTTP 请求超时
  │  │
  │  ├── AppendEvent(event)
  │  │   ├── SerializeEvent(event) → JSON
  │  │   └── HttpPost("/memory/event", json) → 响应
  │  │
  │  ├── BuildContext(request)
  │  │   ├── SerializeContextRequest(request) → JSON
  │  │   ├── HttpPost("/memory/context", json) → 响应 JSON
  │  │   └── DeserializeContextPackage(json) → MemoryContextPackage
  │  │
  │  ├── WritePayload / ReadPayload / Consolidate / SearchMemory / GetStats
  │  │   └── 同样的 POST/GET + 序列化/反序列化模式
```

### 5.3 HTTP 接口设计

| 方法 | HTTP 方法 | 路径 | 说明 |
|------|----------|------|------|
| AppendEvent | POST | `/memory/event` | 追加事件 |
| BuildContext | POST | `/memory/context` | 构建上下文 |
| WritePayload | POST | `/memory/payload/write` | 写入 payload |
| ReadPayload | GET | `/memory/payload/{uri}` | 读取 payload |
| Consolidate | POST | `/memory/consolidate` | 执行整合 |
| SearchMemory | POST | `/memory/search` | 搜索记忆 |
| GetStats | GET | `/memory/stats` | 获取统计 |

### 5.4 错误处理与容错

HttpMemoryRuntime 实现三层容错机制：

1. **自动重试**：HttpPost / HttpGet 对瞬态错误（HTTP 5xx / 429、curl timeout / connection）自动重试，复用 `retry_helper.h` 的 exponential backoff + jitter。重试次数由 `serverMaxRetries` 配置（默认 2）。不可重试错误（HTTP 4xx / auth failure）不重试。

2. **Circuit breaker 熔断**：连续失败达到 `serverCircuitThreshold`（默认 5）次后打开熔断，`serverCircuitCooldownSeconds`（默认 30s）内所有请求直接返回失败而不发起 HTTP 连接，避免雪崩。成功一次自动关闭熔断。Circuit breaker 状态由 `mutable CircuitState` 结构体维护（含 `atomic<int>` 计数和 `atomic<long long>` 时间戳），在 const 方法内通过 mutable 访问。

3. **失败计数指标**：MemoryStats 新增 `appendFailures` / `writeFailures` / `buildContextFailures` 字段，两个 Runtime 实现各自维护 `atomic<int>` 计数器，在 `GetStats()` 中合并到返回值，使"系统故障"与"无数据"可区分。

## 6. Payload Offloading 设计

### 6.1 设计意图

大型工具结果（如 `exec` 命令输出、`read_file` 大文件内容）会占用大量上下文 token。Payload Offloading 将这些内容写入 MemoryRuntime，在上下文中只保留简短摘要 + payloadRef，模型可通过 `memory_read_payload` 工具按需读取。

### 6.2 Offload 流程

```
ReactAgentWorker::ReactLoop (工具执行后)
  │  1. 执行工具 → result (字符串)
  │  2. 检查是否需要 offload:
  │     ├── 若 memoryRuntime_ 存在
  │     │   && enablePayloadOffload
  │     │   && result.length() >= offloadToolResultChars
  │     │   → 需要 offload
  │     ├── 否则 → 不 offload
  │  │
  │  3. Offload 流程:
  │     ├── MemoryPayloadWriteRequest{
  │     │       agentId, sessionId, content=result,
  │     │       contentType="text/plain", toolCallId, toolName}
  │     ├── memoryRuntime_->WritePayload(request)
  │     │   → MemoryPayloadWriteResult{
  │     │       succeeded, offloaded, payload, replacementContent}
  │     ├── 若 offload 成功:
  │     │   ├── tool result content = replacementContent (摘要)
  │     │   ├── Message.payloadRef = payload.uri
  │     │   └── 上下文 token 大幅减少
  │     ├── 若 offload 失败:
  │     │   ├── tool result content = 原始 result
  │     │   └── 按正常方式添加到上下文
  │
  │  4. 添加 tool result 消息到 ContextEngine
```

### 6.3 On-demand 读取

当模型需要查看完整的 offloaded 内容时，调用 `memory_read_payload` 工具：

```
Model 输出: tool_calls: [{"name": "memory_read_payload", "arguments": {"uri": "payload://..."}}]
  │
  ▼
MemoryReadPayloadTool::Invoke(input)
  │  ├── 解析 input → uri
  │  ├── memoryRuntime_->ReadPayload(uri)
  │  └── 返回完整 payload 内容
```

### 6.4 配置参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `enablePayloadOffload` | true | 是否启用 offload |
| `offloadToolResultChars` | 8000 | 触发 offload 的最小字符数 |
| `tokenBudget` | 4096 | BuildContext 的 token 预算 |

## 7. 事件流与上下文构建流程

### 7.1 事件流

```
用户/Agent 交互
  │
  ▼
ContextEngine::AddMessage(message)
  │  ├── 添加到内存缓冲区
  │  ├── 持久化到存储后端
  │  └── memoryEventSink_(MemoryEvent)
  │
  ▼
MemoryRuntime::AppendEvent(event)
  │  ├── BuiltinMemoryRuntime → impl_->AppendEvent()
  │  │   → agent-memory-cpp 存储到 events 表
  │  ├── HttpMemoryRuntime → HTTP POST 到服务器
```

### 7.2 上下文构建

```
AgentWorker::BuildPrompt
  │  ├── "{$memory}" → LoadMemoryContent()
  │  │   → memoryContextProvider_()
  │
  ▼
MemoryRuntime::BuildContext(request)
  │  ├── request: {agentId, sessionId, query, tokenBudget}
  │  │
  │  ├── BuiltinMemoryRuntime:
  │  │   ├── impl_->BuildContext(agentRequest)
  │  │   │   → 从 events 表提取相关消息
  │  │   │   → 从 entities/relations 表提取相关实体
  │  │   │   → 在 tokenBudget 内组织内容
  │  │   │   → 返回 agent_memory::MemoryContextPackage
  │  │   └── FromAgentContextPackage() → jiuwen 类型
  │  │
  │  ├── HttpMemoryRuntime:
  │  │   ├── HTTP POST /memory/context
  │  │   ├── DeserializeContextPackage() → jiuwen 类型
  │
  ▼
memoryText 注入系统提示
  │  ├── 包含长期记忆摘要
  │  ├── 包含相关实体和关系
  │  └── 包含 payload 引用
```

### 7.3 整合流程

`Consolidate` 有两个重载，镜像 agent-memory-cpp 的接口设计：

- **`Consolidate(request)`**：runtime 自决模型来源。Builtin 用配置的内建模型（否则规则提取）；HTTP 整合在 server 端用 server 的模型。
- **`Consolidate(request, modelClient)`**：显式指定模型。Builtin 用宿主注入的 modelClient；HTTP 忽略此参数（client 进程内的模型无法给远端 server 使用）。

```
Agent::ConsolidationLoop (后台线程)
  │  ├── 等待会话空闲
  │
  ▼
MemoryRuntime::Consolidate(request, modelClient)
  │  ├── request: {agentId, sessionId, maxEvents}
  │  ├── modelClient: HostMemoryModelClient (适配框架 Model)
  │  │
  │  ├── BuiltinMemoryRuntime:
  │  │   ├── impl_->Consolidate(agentRequest, modelClientAdapter)
  │  │   │   ├── 读取近期 events
  │  │   │   ├── 使用 modelClient 调用 LLM 分析
  │  │   │   ├── 提取新实体和关系
  │  │   │   ├── 更新 entities/relations 表
  │  │   │   └── 标记 events 为已整合
  │  │
  │  ├── HttpMemoryRuntime:
  │  │   ├── HTTP POST /memory/consolidate
  │  │   └── modelClient 不使用（server 有自己的模型），WARN 提示后调用无参重载
```

## 8. 双记忆架构

框架支持两套记忆系统，按 `MemoryConfig.enabled` 选择：

| 条件 | 系统 | 组件 | 存储格式 |
|------|------|------|---------|
| enabled = true | MemoryRuntime | BuiltinMemoryRuntime / HttpMemoryRuntime | 结构化（events + entities + relations + payloads） |
| enabled = false | Legacy Dream | HistoryStore + DreamProcessor | 平面文件（MEMORY.md / SOUL.md / USER.md） |

切换逻辑在 `Agent::ConsolidationLoop` 中：

```cpp
if (memoryRuntime_) {
    // 使用 MemoryRuntime 整合
    memoryRuntime_->Consolidate(request, modelClient);
} else {
    // 使用遗留 Dream 整合
    longTermConsolidator_->Run(model, historyStore);
}
```

## 9. 动态插件扩展

### 9.1 插件加载

```cpp
ResourceManager::LoadMemoryPlugins(pluginDir)
  │  ├── 扫描目录下的 .so / .dll 文件
  │  ├── dlopen / LoadLibrary 加载
  │  ├── dlsym /GetProcAddress 查找 "RegisterMemoryPlugin" 符号
  │  │   └── 符号缺失 → 关闭 handle（避免坏插件泄漏），跳过
  │  ├── 调用 RegisterMemoryPlugin(resourceManager)（锁外调用，避免死锁）
  │  │   → 插件注册自己的 MemoryRuntime provider
  │  ├── 保存 handle 到 pluginHandles_，记录新增 provider 到 pluginProviders_
  │  └── 错误处理: 日志记录，跳过坏插件
```

插件 handle 由 `pluginHandles_` 持有，`UnloadMemoryPlugins()`（析构函数自动调用）按"先 erase 插件 provider、再 dlclose handle"的顺序关闭，避免析构残留工厂 lambda 时访问已卸载代码。详见 `resource_manager_design.md` 6.4 节。

### 9.2 插件接口

```cpp
extern "C" AGENT_PLUGIN_API void RegisterMemoryPlugin(ResourceManager& rm);
```

插件必须：
- 导出 `RegisterMemoryPlugin` 函数
- 在函数中调用 `rm.RegisterMemoryRuntime(provider, factory)`
- 工厂返回 `unique_ptr<MemoryRuntime>` 实现类
- 实现类依赖 `MemoryModelClient` 而非框架 `Model`

### 9.3 AGENT_PLUGIN_API 宏

```cpp
// Linux
#define AGENT_PLUGIN_API __attribute__((visibility("default")))

// Windows
#define AGENT_PLUGIN_API __declspec(dllexport)
```

## 10. 配置参数

### 10.1 MemoryConfig 完整字段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `enabled` | bool | true | 是否启用记忆系统 |
| `mode` | string | "sdk" | 运行模式（"sdk" 进程内 / "server" 远程） |
| `provider` | string | "builtin.compat" | provider 标识 |
| `dataPath` | string | - | 数据存储路径 |
| `serverUrl` | string | - | 远程服务器 URL（server 模式） |
| `serverApiKey` | string | - | 远程服务器认证密钥 |
| `serverTimeoutSeconds` | int | 10 | HTTP 请求超时 |
| `serverMaxRetries` | int | 2 | HTTP 瞬态错误重试次数 |
| `serverCircuitThreshold` | int | 5 | 连续失败达到此数打开熔断 |
| `serverCircuitCooldownSeconds` | int | 30 | 熔断冷却时间 |
| `tokenBudget` | int | 4096 | BuildContext 的 token 预算 |
| `offloadToolResultChars` | int | 8000 | Payload offload 触发阈值 |
| `enablePayloadOffload` | bool | true | 是否启用 payload offload |
| `modelEnabled` | bool | false | 运行时是否自拥模型 |
| `modelFormatType` | string | "openai" | 运行时自拥模型格式 |
| `modelBaseUrl` | string | - | 运行时自拥模型端点 |
| `modelApiKey` | string | - | 运行时自拥模型密钥 |
| `modelName` | string | - | 运行时自拥模型名称 |
| `extraParams` | ConfigNode | - | 运行时自拥模型扩展参数 |

### 10.2 运行时自拥模型

`MemoryConfig.modelEnabled` 控制运行时是否加载自己的 LLM 客户端：

- **false（默认）**：整合时由 `Agent::ConsolidationLoop` 注入 `HostMemoryModelClient`
- **true**：运行时额外加载一个独立模型，用于自主整合

自拥模型适用于记忆服务独立部署（如 HttpMemoryRuntime 侧车）的场景，此时没有框架 Agent 可注入 modelClient。

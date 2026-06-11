# Memory System Architecture

## 总体架构

Memory System 设计为独立 Memory Runtime，jiuwen-lite 通过 Adapter 接入。

```text
+-------------------+        +-----------------------+
| jiuwen-lite       |        | openclaw / Hermes     |
|                   |        | other agent runtime   |
| MemoryAdapter     |        | MemoryAdapter         |
+---------+---------+        +-----------+-----------+
          |                              |
          | Memory Protocol              | Memory Protocol
          v                              v
+----------------------------------------------------+
| Memory Runtime                                     |
|                                                    |
|  +----------------+  +--------------------------+  |
|  | Memory API     |  | Memory Policy            |  |
|  +----------------+  +--------------------------+  |
|  | Event Store    |  | Short-Term Memory        |  |
|  | Payload Store  |  | Long-Term Memory         |  |
|  | Summary Store  |  | Entity Graph             |  |
|  | Entity Store   |  | Context Builder          |  |
|  +----------------+  +--------------------------+  |
|                                                    |
|  +----------------+  +--------------------------+  |
|  | Model Adapter  |  | Plugin Interfaces        |  |
|  +----------------+  +--------------------------+  |
+----------------------------------------------------+
          |
          v
+-------------------+
| Storage Backend   |
| SQLite / File     |
| JSON / Vector     |
+-------------------+
```

## jiuwen-lite 内部边界

### ContextEngine

职责：

- 保存和读取会话消息。
- 维护消息结构合法性。
- 执行上下文组装。
- 执行 token budget 分配。
- 调用 MemoryAdapter 获取短期和长期记忆片段。

不再负责：

- 直接读取长期记忆文件。
- 执行记忆提炼。
- 管理 Dream consolidation。
- 判断哪些内容应进入长期记忆。

当前需要逐步迁移的点：

- `src/context_engine/context_engine.cpp:101` 的上下文裁剪逻辑后续改为可接收 Memory Runtime 的上下文包。
- `src/context_engine/context_engine.cpp:263` 的 `GetMemoryContent` 后续改为通过 MemoryAdapter 获取。

### Agent

职责：

- 管理 Agent 生命周期。
- 将会话开始、会话结束、用户消息、助手消息等事件转发给 MemoryAdapter。
- 触发后台 consolidation。

不再负责：

- 直接持有具体 `DreamProcessor`。
- 直接管理长期记忆文件。

当前需要逐步迁移的点：

- `src/core/agent.cpp:78` 的 `HistoryStore` 后续迁移到 Memory Runtime 的 EventStore。
- `src/core/agent.cpp:81` 的 `DreamProcessor` 后续迁移到 Memory Runtime 的 LongTermConsolidator。

### AgentWorker / ReactWorker

职责：

- 执行模型调用和工具调用。
- 在工具执行前后发送 memory event。
- 工具结果写入上下文前交由 MemoryAdapter 判断是否卸载。

当前需要逐步迁移的点：

- `src/workers/react_worker.cpp:141` 工具结果产生后，先调用 MemoryAdapter 进行 payload offload / summarization，再写入 ContextEngine。
- `src/workers/react_worker.cpp:148` 构造 tool message 时，应支持 payload ref metadata。

## Memory Runtime 模块

### Memory API

对外稳定接口。

```text
AppendEvent(event)
BuildContext(request)
ReadPayload(ref)
Consolidate(request)
SearchMemory(query)
GetMemoryStats()
```

### Event Store

保存原始事件流。

事件包括：

- session started
- session ended
- message appended
- tool call started
- tool call finished
- payload offloaded
- consolidation completed

建议第一版使用 SQLite 或 JSONL。

### Payload Store

保存大段内容。

典型内容：

- 大工具执行结果。
- 大文件内容。
- 网页抓取结果。
- 搜索结果集合。
- 长日志。

Payload ref 示例：

```text
payload://agent/{agentId}/session/{sessionId}/tool/{toolCallId}
```

Payload 元数据：

```json
{
  "ref": "payload://agent/a/session/s/tool/call_1",
  "contentType": "tool_result",
  "toolName": "grep",
  "originalChars": 48231,
  "summary": "grep found 24 matches in src/core",
  "createdAt": "2026-06-07T00:00:00Z"
}
```

### Short-Term Memory

负责短期上下文保真和降 token。

分层：

```text
Hot Buffer
  最近完整消息

Warm Summary
  滚动压缩后的会话片段

Cold Payload
  大内容外置存储，通过 ref 引用
```

策略：

- 最近 N 条消息保持完整。
- 超过 token 阈值后对旧消息生成 rolling summary。
- 超过字符阈值的工具结果写入 Payload Store。
- 上下文中默认放 summary + ref。
- 需要细节时通过 `ReadPayload` 或 memory tool 读取。

### Long-Term Memory

负责跨会话、跨任务的稳定记忆。

分层：

```text
L0 Raw Events
L1 Session Summary
L2 Topic Summary
L3 Profile Memory
Entity Graph
```

长期记忆条目字段：

```json
{
  "id": "memory:profile:cpp_style",
  "type": "preference",
  "summary": "User prefers Allman function style and K&R control flow.",
  "confidence": 0.92,
  "sourceRefs": ["event://123", "summary://session/s1"],
  "createdAt": "...",
  "updatedAt": "...",
  "expiresAt": null
}
```

### LLM Memory Extraction

长期记忆提取采用 LLM 优先、规则兜底：

- `BuiltinMemoryRuntime::Consolidate(request, model)` 在收到 `Model*` 时调用 `LLMLongTermMemoryProcessor`。
- LLM processor 通过 `Model::Format(systemPrompt, messages, tools)` 和 `Model::Invoke(formattedInput, onChunk)` 复用宿主模型能力。
- LLM 必须返回严格 JSON；解析失败、调用失败或无有效提取结果时，回退 `RuleBasedLongTermMemoryProcessor`。
- 外部 memory-server / MCP server 没有宿主模型时仍可使用规则兜底 consolidation。

LLM 输出 schema：

```json
{
  "topicSummaries": ["brief topic summary"],
  "profileSummaries": ["stable user preference or profile insight"],
  "entities": [
    {
      "id": "entity:preference.concise_answers",
      "type": "preference",
      "name": "Concise answers",
      "summary": "User prefers concise answers",
      "confidence": 0.9,
      "sourceRefs": ["event://1"]
    }
  ],
  "relations": [
    {
      "fromEntity": "entity:user",
      "relation": "prefers",
      "toEntity": "entity:preference.concise_answers",
      "confidence": 0.85,
      "sourceRefs": ["event://1"]
    }
  ]
}
```

### Conflict / Supersede / Obsolete

长期记忆采用 active/inactive 标记处理冲突和替代：

- `memory_entities.active = 1` 表示当前有效实体。
- `memory_entities.active = 0` 表示 obsolete 实体。
- `memory_entities.superseded_by` 指向替代它的新实体。
- `memory_relations.active = 1` 表示当前有效关系。
- 同一 `(from_entity, relation)` 写入新关系时，旧关系会标记 inactive。
- 写入 `supersedes` 关系时，`toEntity` 会被标记 obsolete，`fromEntity` 成为替代实体。
- `BuildContext` 只加载 active 实体和 active 关系，避免过期偏好污染上下文。
- 同 ID entity 更新时保留 previous summary，便于后续审计和冲突解释。

### Entity Graph

实体类型：

- user
- agent
- project
- file
- task
- preference
- skill
- tool
- topic
- memory

关系类型：

- prefers
- works_on
- mentions
- depends_on
- contradicts
- supersedes
- derived_from
- related_to

关系示例：

```json
{
  "from": "entity:user",
  "relation": "prefers",
  "to": "entity:cpp_style",
  "confidence": 0.9,
  "sourceRefs": ["event://123"]
}
```

### Context Builder

输入：

```json
{
  "agentId": "agent-a",
  "sessionId": "s1",
  "query": "...",
  "tokenBudget": 12000,
  "include": ["short_term", "long_term", "entities", "payload_refs"]
}
```

输出：

```json
{
  "messages": [],
  "memoryText": "...",
  "entities": [],
  "payloadRefs": [],
  "citations": []
}
```

预算建议：

```text
system prompt       20%
recent messages     40%
short summaries     15%
long-term memory    15%
tool schemas        10%
```

实际比例应可配置。

### Memory Policy

策略包括：

- tool result offload threshold
- max hot messages
- max short-term summary tokens
- max long-term memory tokens
- sensitive data handling
- retention days
- consolidation interval
- conflict resolution

### Model Adapter

Memory Runtime 需要调用模型执行：

- 摘要。
- 事实抽取。
- 实体抽取。
- 关系抽取。
- 冲突判断。

Model Adapter 不应绑定 jiuwen-lite 的 Model 类型。应定义独立接口，并由各宿主提供实现或配置外部 LLM API。

```text
FormatPrompt
Invoke
InvokeJson
```

## 插件接口

Memory Runtime 内部未来可开放以下插件：

```text
MemoryStorePlugin
PayloadStorePlugin
CompressorPlugin
SummarizerPlugin
EntityExtractorPlugin
RetrieverPlugin
PolicyPlugin
ModelProviderPlugin
```

第一阶段先实现内置默认插件，接口预留即可。

## 数据目录建议

jiuwen-lite 默认本地目录：

```text
data/
  memory_runtime/
    memory.db
    payloads/
    summaries/
    indexes/
```

兼容旧目录：

```text
data/memory/MEMORY.md
data/SOUL.md
data/USER.md
```

迁移期默认同时支持旧文件读取。

## SQLite 表建议

第一版建议以 SQLite 为主，文件系统存 payload。

```text
memory_events
memory_payloads
memory_summaries
memory_entities
memory_relations
memory_cursors
memory_sources
```

### memory_events

```text
id
agent_id
session_id
event_type
role
content
payload_ref
tool_call_id
tool_name
metadata_json
created_at
```

### memory_payloads

```text
ref
agent_id
session_id
content_type
path
summary
original_chars
metadata_json
created_at
```

### memory_summaries

```text
id
agent_id
session_id
level
topic
summary
source_refs_json
confidence
created_at
updated_at
```

### memory_entities

```text
id
type
name
summary
confidence
metadata_json
created_at
updated_at
```

### memory_relations

```text
id
from_entity
relation
to_entity
confidence
source_refs_json
created_at
updated_at
```

## 独立仓库与 API 交付形态

Memory Runtime 后续应拆分为独立仓库，jiuwen-lite 只保留 adapter/client 集成层。独立仓建议提供三种接入方式：

```text
memory-runtime/
  include/               C++ SDK public headers
  src/                   SDK/runtime implementation
  server/                HTTP memory-server
  mcp/                   stdio MCP server
  adapters/              framework adapters
  examples/              SDK / HTTP / MCP examples
```

API 形态：

- C++ SDK：进程内 `MemoryRuntime`，适合 jiuwen-lite 默认接入。
- HTTP server：独立 `memory-server`，适合多语言 Agent 和跨进程共享记忆。
- MCP server：`memory-mcp-server`，适合支持 MCP 的 Agent 框架。

jiuwen-lite 接入策略：

- 默认使用 SDK 模式：`memoryConfig.mode = "sdk"`。
- 可配置使用 server 模式：`memoryConfig.mode = "server"`。
- server 模式下 jiuwen-lite 创建 `HttpMemoryRuntime`，通过 HTTP 调用 memory-server。

`agents.json` server 模式示例见 `examples/memory_server/agents_server_mode.example.json`。

## jiuwen-lite 适配层

建议新增：

```text
include/memory_runtime.h
include/memory_types.h
include/memory_adapter.h
src/memory/
```

核心类：

```cpp
class MemoryRuntime {
public:
    virtual bool AppendEvent(const MemoryEvent& event) = 0;
    virtual MemoryContextPackage BuildContext(const MemoryContextRequest& request) = 0;
    virtual MemoryPayloadWriteResult WritePayload(const MemoryPayloadWriteRequest& request) = 0;
    virtual std::string ReadPayload(const std::string& ref) = 0;
    virtual bool Consolidate(const MemoryConsolidationRequest& request) = 0;
    virtual bool Consolidate(const MemoryConsolidationRequest& request, Model* model);
};

class MemoryAdapter {
public:
    void OnMessageAppended(const Message& message);
    Message ProcessToolResult(const Message& toolMessage);
    MemoryContextPackage BuildContext(...);
};
```

## MCP / Server 形态

Memory Runtime 后续提供 MCP server：

工具：

```text
memory_append_event
memory_build_context
memory_search
memory_read_payload
memory_consolidate
memory_stats
```

### Server-side LLM 配置

`memory-server` 与 `memory-mcp-server` 支持在启动时加载模型配置：

```bash
memory-server --data ./data --model-config ./model.json
memory-mcp-server --data ./data --model-config ./model.json
```

`model.json` 示例：

```json
{
  "formatType": "openai",
  "provider": "",
  "baseUrl": "https://api.example.com/v1/chat/completions",
  "apiKey": "<your api key>",
  "modelName": "your-model-name"
}
```

模型由 server 启动时创建并持有；HTTP `/v1/consolidate` 与 MCP `memory_consolidate` 调用方不需要传模型参数。未配置模型时，consolidation 继续使用规则兜底路径。

### MCP 工具 schema

#### memory_append_event

```json
{
  "name": "memory_append_event",
  "description": "Append a memory event to Memory Runtime.",
  "inputSchema": {
    "type": "object",
    "required": ["type", "agentId", "sessionId"],
    "properties": {
      "type": { "type": "integer" },
      "agentId": { "type": "string" },
      "sessionId": { "type": "string" },
      "role": { "type": "string" },
      "content": { "type": "string" },
      "toolCallId": { "type": "string" },
      "toolName": { "type": "string" },
      "payloadRef": { "type": "string" }
    }
  }
}
```

#### memory_build_context

```json
{
  "name": "memory_build_context",
  "description": "Build short-term and long-term memory context for an agent request.",
  "inputSchema": {
    "type": "object",
    "required": ["agentId", "sessionId"],
    "properties": {
      "agentId": { "type": "string" },
      "sessionId": { "type": "string" },
      "query": { "type": "string" },
      "tokenBudget": { "type": "integer" },
      "include": {
        "type": "array",
        "items": { "type": "string" }
      }
    }
  }
}
```

#### memory_read_payload

```json
{
  "name": "memory_read_payload",
  "description": "Read full content from an offloaded memory payload reference.",
  "inputSchema": {
    "type": "object",
    "required": ["ref"],
    "properties": {
      "ref": { "type": "string" }
    }
  }
}
```

#### memory_consolidate

```json
{
  "name": "memory_consolidate",
  "description": "Trigger memory consolidation for an agent/session.",
  "inputSchema": {
    "type": "object",
    "required": ["agentId"],
    "properties": {
      "agentId": { "type": "string" },
      "sessionId": { "type": "string" },
      "maxEvents": { "type": "integer" },
      "force": { "type": "boolean" }
    }
  }
}
```

#### memory_search

```json
{
  "name": "memory_search",
  "description": "Search long-term memory.",
  "inputSchema": {
    "type": "object",
    "required": ["query"],
    "properties": {
      "agentId": { "type": "string" },
      "sessionId": { "type": "string" },
      "query": { "type": "string" },
      "limit": { "type": "integer" }
    }
  }
}
```

#### memory_stats

```json
{
  "name": "memory_stats",
  "description": "Return Memory Runtime statistics.",
  "inputSchema": {
    "type": "object",
    "properties": {}
  }
}
```

第一阶段 MCP server 建议以 stdio transport 实现，复用 `MemoryRuntime` 与 HTTP server 的 JSON 转换逻辑。

HTTP API：

```text
POST /v1/events
POST /v1/context
GET  /v1/payloads/{ref}
POST /v1/consolidate
POST /v1/search
GET  /v1/stats
```

第一阶段 HTTP JSON schema：

```text
POST /v1/events
  request:  MemoryEvent JSON
  response: {"ok": true}

POST /v1/context
  request:  MemoryContextRequest JSON
  response: MemoryContextPackage JSON

POST /v1/payloads
  request:  MemoryPayloadWriteRequest JSON
  response: MemoryPayloadWriteResult JSON

GET /v1/payloads/{encodedRef}
  response: {"ok": true, "ref": "...", "content": "..."}

POST /v1/consolidate
  request:  MemoryConsolidationRequest JSON
  response: {"ok": true, "handled": true|false}

POST /v1/search
  request:  MemorySearchRequest JSON
  response: {"ok": true, "results": [...]}

GET /v1/stats
  response: MemoryStats JSON
```

## 外部框架接入

### Generic REST Adapter

任意 Agent 框架接入 Memory Runtime 时，只需要完成三类映射：

```text
AgentMessage -> MemoryEvent
ToolResult   -> MemoryPayloadWriteRequest 或 MemoryEvent
MemoryContextPackage -> AgentPrompt / Messages
```

启动 memory-server：

```bash
memory-server --host 127.0.0.1 --port 8090 --data ./data --model-config ./model_config.local.json
```

配置文件可从 `examples/memory_server/model_config.example.json` 复制：

```bash
cp examples/memory_server/model_config.example.json model_config.local.json
```

`model_config.local.json` 已加入 `.gitignore`，应填入真实 `baseUrl`、`apiKey`、`modelName`。

jiuwen-lite 使用 server 模式时，在 `agents.json` 的 agent 配置中加入：

```json
{
  "memoryConfig": {
    "enabled": true,
    "mode": "server",
    "serverUrl": "http://127.0.0.1:8090",
    "serverApiKey": "<optional memory server api key>",
    "serverTimeoutSeconds": 10,
    "enablePayloadOffload": true,
    "enableHierarchicalSummary": true,
    "enableEntityGraph": true
  }
}
```

推荐接入流程：

```text
1. 会话开始
   可选：POST /v1/events 写入 session.started

2. 用户/助手/工具消息产生后
   POST /v1/events

3. 工具结果过大时
   可由宿主先调用 Memory Runtime SDK 的 WritePayload
   或未来通过 HTTP payload write API 写入

4. 模型调用前
   POST /v1/context
   将返回的 memoryText 注入 system/developer/context message

5. 会话结束或后台任务
   POST /v1/consolidate

6. 需要检索时
   POST /v1/search

7. 需要展开冷数据时
   GET /v1/payloads/{ref}
```

最小 HTTP 调用示例：

```bash
curl -X POST http://127.0.0.1:8090/v1/events \
  -H 'Content-Type: application/json' \
  -d '{"type":2,"agentId":"agent-1","sessionId":"session-1","role":"user","content":"I prefer concise answers"}'

curl -X POST http://127.0.0.1:8090/v1/context \
  -H 'Content-Type: application/json' \
  -d '{"agentId":"agent-1","sessionId":"session-1","query":"answer the user"}'

curl -X POST http://127.0.0.1:8090/v1/consolidate \
  -H 'Content-Type: application/json' \
  -d '{"agentId":"agent-1","sessionId":"session-1","force":true}'
```

通用事件映射建议：

```json
{
  "type": 2,
  "agentId": "agent-id",
  "sessionId": "session-id",
  "role": "user|assistant|tool|system",
  "content": "message content",
  "toolCallId": "optional-tool-call-id",
  "toolName": "optional-tool-name",
  "payloadRef": "optional-payload-ref"
}
```

上下文注入建议：

```text
System Prompt
  + Framework native instructions
  + Memory Runtime memoryText
  + Recent messages
  + Tool schemas
```

适配层应保证：

- 不依赖 jiuwen-lite 的 `Message` 类型。
- 不假设 OpenAI/Anthropic 消息格式。
- 保留 sessionId、agentId、toolCallId、payloadRef。
- Memory Runtime 失败时降级为框架自身最近消息窗口。
- 大 payload 默认不展开，只有模型或框架明确请求时再读取。

### openclaw Adapter 草案

openclaw 接入可优先使用 sidecar 模式：

```text
openclaw runtime
  -> REST adapter
  -> memory-server
```

openclaw adapter 需要实现：

```text
on_message(message)       -> POST /v1/events
before_model_call(query)  -> POST /v1/context
after_session_end()       -> POST /v1/consolidate
read_payload(ref)         -> GET /v1/payloads/{ref}
```

### Hermes Adapter 草案

Hermes 如果支持 MCP，优先使用 MCP server：

```text
Hermes
  -> memory-mcp-server stdio
```

Hermes adapter 需要调用：

```text
memory_append_event
memory_build_context
memory_consolidate
memory_search
memory_read_payload
memory_stats
```

如果 Hermes 不使用 MCP，也可以使用 Generic REST Adapter。

## 独立仓交付规划

### 目录结构

Memory Runtime 独立仓建议采用以下最小目录结构：

```text
memory-runtime/
  CMakeLists.txt
  README.md
  include/
    memory_types.h
    memory_runtime.h
    memory_adapter.h
  src/
    memory/
      builtin_memory_runtime.cpp
      builtin_memory_runtime.h
      http_memory_runtime.cpp
      http_memory_runtime.h
      long_term_memory_processor.cpp
      long_term_memory_processor.h
      memory_sqlite_store.cpp
      memory_sqlite_store.h
    server/
      main.cpp
      mcp_main.cpp
  examples/
    memory_server/
      agents_server_mode.example.json
      model_config.example.json
  tests/
    test_memory_runtime.cpp
  scripts/
    build_linux.sh
```

jiuwen-lite 仓内当前对应文件：

```text
include/memory_types.h
include/memory_runtime.h
include/memory_adapter.h
src/memory/*
src/memory_server/main.cpp
src/memory_server/mcp_main.cpp
examples/memory_server/*
unittest/test_memory_runtime.cpp
build_memory_runtime_linux.sh
```

当前仓内 memory-only 构建入口为：

```bash
./build_memory_runtime_linux.sh
```

产物输出到：

```text
dist/memory-runtime/linux/
  bin/memory-server
  bin/memory-mcp-server
  lib/libagent_framework.so
  include/*
  examples/memory_server/*
```

### 最小依赖边界

独立仓第一版应将 Memory Runtime 边界收敛为：

- C++17 标准库。
- SQLite3：事件、payload、summary、entity、relation 持久化。
- nlohmann/json：HTTP/MCP JSON 协议与 SQLite JSON 字段。
- libcurl：仅 `HttpMemoryRuntime` 需要。
- cpp-httplib：仅 `memory-server` 需要。
- 可选模型接口：仅 LLM consolidation 需要；无模型时必须可规则兜底。

需要从 jiuwen-lite 解耦的类型：

- `MemoryConfig` 当前在 `include/types.h`，独立仓应迁移到 `include/memory_types.h` 或独立 `memory_config.h`。
- `Message` 当前来自 `include/model.h`，独立仓应定义 `MemoryMessage` 或让 `MemoryContextPackage` 只返回纯 memory blocks。
- `Model` 当前来自 `include/model.h`，独立仓应改为轻量 `MemoryModelClient` 接口，只暴露 `InvokeMemoryExtraction(prompt)`。
- `AGENT_API` 当前来自 `include/agent_export.h`，独立仓应替换为 `MEMORY_API`。
- `ResourceManager` 只属于 jiuwen-lite 宿主接入层，不应进入独立 runtime core。

独立仓建议拆成 4 个 target：

```text
memory_runtime_core      # types + runtime interface + builtin runtime + sqlite store
memory_runtime_http      # HttpMemoryRuntime client
memory-server            # REST sidecar
memory-mcp-server        # MCP stdio sidecar
```

依赖方向：

```text
memory_runtime_core <- memory_runtime_http
memory_runtime_core <- memory-server
memory_runtime_core <- memory-mcp-server
jiuwen-lite         -> memory_runtime_core 或 memory_runtime_http
```

### 独立交付 README 草案

README 应覆盖三种使用方式：

1. SDK 模式：C++ 宿主直接创建 `BuiltinMemoryRuntime`，调用 `AppendEvent`、`WritePayload`、`BuildContext`、`Consolidate`。
2. HTTP 模式：启动 `memory-server`，任意语言通过 REST API 接入。
3. MCP 模式：启动 `memory-mcp-server`，支持 MCP 的 Agent 通过工具调用接入。

最小启动命令：

```bash
./bin/memory-server --host 127.0.0.1 --port 8090 --data ./data
./bin/memory-mcp-server --data ./data
```

可选 LLM consolidation：

```bash
cp examples/memory_server/model_config.example.json model_config.local.json
./bin/memory-server --data ./data --model-config ./model_config.local.json
```

最小 HTTP 示例：

```bash
curl -X POST http://127.0.0.1:8090/v1/events \
  -H 'Content-Type: application/json' \
  -d '{"type":2,"agentId":"agent-1","sessionId":"session-1","role":"user","content":"I prefer concise answers"}'

curl -X POST http://127.0.0.1:8090/v1/consolidate \
  -H 'Content-Type: application/json' \
  -d '{"agentId":"agent-1","sessionId":"session-1","force":true}'

curl -X POST http://127.0.0.1:8090/v1/context \
  -H 'Content-Type: application/json' \
  -d '{"agentId":"agent-1","sessionId":"session-1","query":"answer the user"}'
```

## 兼容策略

### 旧 Dream 兼容

第一阶段将现有 DreamProcessor 包装为默认 LongTermConsolidator，不立即删除。

### 旧 Memory 文件兼容

继续读取：

```text
MEMORY.md
SOUL.md
USER.md
```

同时逐步写入新的 SQLite store。

### 降级策略

Memory Runtime 不可用时：

- 使用 ContextEngine 当前最近消息窗口。
- 跳过长期记忆。
- 工具结果不卸载。
- 记录日志但不中断主流程。

## 实施路线

### Phase 0：文档和边界确认

- 完成 vision、architecture、progress 文档。
- 明确 Memory Runtime 独立交付目标。

### Phase 1：协议和接口

- 新增 memory types。
- 新增 MemoryRuntime 抽象。
- 新增 MemoryAdapter。
- ResourceManager 支持注册 memory runtime provider。

### Phase 2：兼容实现

- 包装现有 HistoryStore。
- 包装现有 DreamProcessor。
- ContextEngine 从 MemoryAdapter 获取长期记忆文本。
- 行为保持兼容。

### Phase 3：短期记忆

- 实现 payload offload。
- 工具结果摘要。
- memory_read_payload 能力。
- Context Builder 支持 payload refs。

### Phase 4：长期记忆

- SQLite schema。
- 分层摘要。
- 实体和关系抽取。
- source refs。

### Phase 5：独立服务

- memory-server。
- HTTP API。
- MCP server。

### Phase 6：外部框架适配

- openclaw adapter。
- Hermes adapter。
- generic REST adapter。

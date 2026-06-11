# Memory System Vision

## 背景

jiuwen-lite 当前已经具备上下文持久化、Dream 记忆整理、Skill、MCP 和工具调用能力。随着 Agent 执行链条变长，单纯依赖上下文窗口裁剪和 Markdown 记忆文件会遇到以下问题：

- 大段工具执行结果占用上下文，影响模型推理效率。
- 长会话中的关键信息容易被简单裁剪丢失。
- 长期记忆以 `MEMORY.md`、`SOUL.md`、`USER.md` 为主，难以表达层级摘要、实体关系和可追溯来源。
- 记忆逻辑与 jiuwen-lite 内部实现耦合，未来难以复用到 openclaw、Hermes 等其他智能体框架。

因此需要将记忆能力抽象为独立系统，jiuwen-lite 通过适配层接入。

## 愿景

构建一个可独立交付的 Memory Runtime，为多种智能体框架提供统一的短期记忆、长期记忆、上下文构建、记忆检索和记忆维护能力。

目标形态：

```text
memory-runtime
  独立协议
  独立存储
  独立配置
  独立运行时
  多传输形态
  多框架适配

jiuwen-lite / openclaw / Hermes / other agents
  通过 adapter 接入 memory-runtime
```

jiuwen-lite 内部的 memory 插件不应承载复杂业务逻辑，而应作为 `MemoryAdapter`，负责把 jiuwen-lite 的消息、工具调用、会话事件转换成 Memory Runtime 的通用事件。

## 设计原则

1. **独立交付**
   - Memory Runtime 不依赖 jiuwen-lite 内部类型。
   - 可以作为 C++ SDK、sidecar 服务或远程服务独立部署。

2. **协议优先**
   - 先定义稳定的 Memory Event、Memory Context、Payload Ref、Entity、Relation 等通用数据结构。
   - jiuwen-lite、openclaw、Hermes 只实现协议适配。

3. **上下文与记忆分离**
   - ContextEngine 主要负责上下文组装、token 预算和消息合法性。
   - Memory Runtime 负责短期记忆、长期记忆、压缩、卸载、检索、摘要和实体关系。

4. **短期记忆保真降噪**
   - 最近消息保留完整内容。
   - 较早消息滚动压缩。
   - 大段工具结果卸载为 payload，通过引用保留可追溯性。

5. **长期记忆结构化**
   - 从原始事件中生成会话摘要、主题摘要、用户画像、项目事实和稳定偏好。
   - 建立实体和关系，保留 source refs，支持纠错和过期。

6. **可插拔实现**
   - Store、Compressor、Summarizer、Entity Extractor、Retriever、Policy 都应可替换。
   - 第一版可以内置默认实现，后续再开放插件机制。

7. **多部署形态**
   - Embedded：进程内 SDK，适合高性能集成。
   - Sidecar：本机 memory-server，适合多语言框架。
   - Remote：共享记忆服务，适合多 Agent 协作。

8. **失败隔离**
   - Memory Runtime 失败不应导致 Agent 主流程不可用。
   - 上下文构建失败时应降级为最近消息窗口。

## 能力范围

### 短期记忆

```text
Hot Buffer
  最近完整消息

Compressed Segments
  被压缩的对话片段

Payload Store
  大工具结果、网页内容、文件内容外置化

Context Builder
  按 token budget 组合最终上下文
```

短期记忆重点能力：

- 最近上下文窗口管理。
- 大段工具结果卸载。
- 工具结果摘要生成。
- payload ref 读取。
- 滚动摘要。
- 按 token 预算组装上下文。

### 长期记忆

```text
L0 Raw Events
  原始事件和 payload refs

L1 Session Summary
  单会话或单阶段摘要

L2 Topic Summary
  主题级聚合摘要

L3 Profile Memory
  用户偏好、项目事实、稳定约束

Entity Graph
  人、项目、文件、任务、偏好、技能之间的关系
```

长期记忆重点能力：

- 分层摘要。
- 事实抽取。
- 用户偏好抽取。
- 项目知识沉淀。
- 实体抽取。
- 实体关系维护。
- source refs 追溯。
- 过期、冲突和置信度管理。

## 通用事件模型

Memory Runtime 不直接依赖具体 Agent 框架，只接收通用事件。

典型事件：

```text
session.started
session.ended
conversation.message.appended
tool.call.started
tool.call.finished
context.requested
memory.consolidation.requested
```

示例：

```json
{
  "eventType": "conversation.message.appended",
  "agentId": "agent-a",
  "sessionId": "s1",
  "role": "tool",
  "content": "...",
  "toolCallId": "call_123",
  "toolName": "grep",
  "timestamp": "2026-06-07T00:00:00Z",
  "metadata": {}
}
```

## 核心 API

最小 API：

```text
AppendEvent(event)
BuildContext(request) -> context package
ReadPayload(ref) -> full content
Consolidate(request)
SearchMemory(query)
GetMemoryStats()
```

其中 `BuildContext` 是最重要的跨框架能力。

请求示例：

```json
{
  "agentId": "agent-a",
  "sessionId": "s1",
  "query": "用户当前问题",
  "tokenBudget": 12000,
  "include": ["short_term", "long_term", "entities", "payload_refs"]
}
```

返回示例：

```json
{
  "messages": [],
  "memoryText": "...",
  "entities": [],
  "payloadRefs": [],
  "citations": []
}
```

## 交付形态

### C++ SDK

```text
libmemory_runtime.so / memory_runtime.dll
```

适合 jiuwen-lite 进程内集成。

### Memory Server

```text
memory-server
  HTTP API
  stdio API
  MCP server
```

适合 openclaw、Hermes、Python/JS/Go 等智能体框架接入。

### 多语言 SDK

后续按需要提供：

```text
C++
Python
TypeScript
Go
```

## 优先级

1. 定义协议和数据结构。
2. 在 jiuwen-lite 中实现 MemoryAdapter 雏形。
3. 把现有 HistoryStore 和 DreamProcessor 包装为默认兼容实现。
4. 实现短期记忆 payload offload。
5. 实现长期分层摘要。
6. 实现实体和关系存储。
7. 提供 memory-server。
8. 适配 openclaw、Hermes 等外部框架。

## 非目标

第一阶段暂不追求：

- 完整 marketplace。
- 复杂分布式存储。
- 强一致多 Agent 共享事务。
- Native 热卸载。
- 高级图数据库强依赖。

第一阶段重点是边界清晰、协议稳定、可独立交付、可逐步替换现有记忆实现。

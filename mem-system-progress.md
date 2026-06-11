# Memory System Progress

## 当前阶段

Phase 7：独立仓库交付准备。

## 总体待办

### Phase 0：文档和边界确认

- [x] 创建 `mem-system-vision.md`
- [x] 创建 `mem-system-architecture.md`
- [x] 创建 `mem-system-progress.md`
- [x] 确认 Memory Runtime 是否作为独立仓库交付
- [x] 确认第一阶段是否仅做 jiuwen-lite 内嵌 SDK 形态
- [x] 确认 memory-server 的优先级

### Phase 1：协议和接口

- [x] 设计 `MemoryEvent`
- [x] 设计 `MemoryContextRequest`
- [x] 设计 `MemoryContextPackage`
- [x] 设计 `MemoryPayloadRef`
- [x] 设计 `MemoryEntity`
- [x] 设计 `MemoryRelation`
- [x] 设计 `MemoryStats`
- [x] 设计 `MemoryRuntime` 抽象接口
- [x] 设计 `MemoryAdapter` 抽象接口
- [x] 设计 `MemoryConfig`
- [x] 在 `AgentConfig` 中增加 memory 配置入口
- [x] 在 `ResourceManager` 中增加 memory runtime provider 注册能力

### Phase 2：兼容实现

- [x] 新增默认 `BuiltinMemoryRuntime`
- [x] 将会话消息追加事件转发到 MemoryRuntime
- [x] 将 MemoryRuntime message events 持久化为兼容 `history.jsonl`
- [x] memory runtime 启用时避免 Agent 旧 HistoryStore 双写
- [x] 将 `MemoryRuntime::Consolidate` 接入后台 consolidation 路径
- [x] `ContextEngine::GetMemoryContent` 改为从 Memory Runtime 获取
- [x] `Agent` 不再直接依赖具体 DreamProcessor
- [x] 保持 `MEMORY.md`、`SOUL.md`、`USER.md` 兼容读取
- [x] 增加兼容测试，确保现有上下文和 Dream 行为不变

### Phase 3：短期记忆

- [x] 设计 payload ref 格式
- [x] 新增 PayloadStore
- [x] 实现大工具结果卸载
- [x] 实现工具结果摘要
- [x] tool message 支持 summary + payload ref
- [x] tool message/event/storage 支持 payloadRef
- [x] 新增 payload read API
- [x] 新增 memory_read_payload 工具或内部能力
- [x] Context Builder 支持 Hot Buffer / Warm Summary / Cold Payload 初步概览
- [x] 增加大工具结果上下文压缩测试

### Phase 4：长期记忆

- [x] 设计 SQLite schema
- [x] 实现 `memory_events`
- [x] 实现 `memory_payloads`
- [x] 实现 `memory_summaries` 表结构与写入 API
- [x] 实现 `memory_entities` 表结构与写入 API
- [x] 实现 `memory_relations` 表结构与写入 API
- [x] 实现 session summary
- [x] 实现 topic summary 初版 LLM 提取，规则兜底
- [x] 实现 profile memory 初版 LLM 提取，规则兜底
- [x] 实现 entity extraction 初版 LLM 提取，规则兜底
- [x] 实现 relation extraction 初版 LLM 提取，规则兜底
- [x] 实现 LLM 提取 prompt 模板、JSON 解析、sanitization
- [x] LLM 提取无效时自动 fallback 到规则型提取
- [x] MemoryRuntime 接口扩展支持可选 Model* 注入
- [x] 实现 source refs
- [x] 实现 conflict / supersede / obsolete 处理
- [x] 增加长期记忆 BuildContext 读取测试
- [x] 增加长期记忆 consolidation 测试

### Phase 5：独立服务

- [x] 设计 HTTP API
- [x] 实现 `POST /v1/events`
- [x] 实现 `POST /v1/context`
- [x] 实现 `GET /v1/payloads/{ref}`
- [x] 实现 `POST /v1/consolidate`
- [x] 实现 `POST /v1/search`
- [x] 实现 `GET /v1/stats`
- [x] 设计 MCP server 工具 schema
- [x] 实现 MCP server 形态
- [x] 增加 server smoke test

### Phase 6：外部框架适配

- [x] 设计 generic REST adapter 指南
- [x] 设计 openclaw adapter
- [x] 设计 Hermes adapter
- [x] 验证 memory-server 可被非 C++ Agent 调用
- [x] 整理跨框架接入文档

### Phase 7：独立仓库交付准备

- [x] 明确 Memory Runtime 拆为独立仓库/包
- [x] 明确 jiuwen-lite 默认 SDK 接入
- [x] 支持 jiuwen-lite server 模式接入
- [x] 提供 `agents.json` 风格的 server 模式配置样例
- [x] 整理独立仓目录结构与构建脚本
- [x] 抽离 memory runtime 最小依赖边界
- [x] 补充 SDK/HTTP/MCP 独立交付 README

## 近期优先级

1. 先完成 Phase 1 的协议和接口。
2. 再做 Phase 2 的兼容实现，确保不破坏现有行为。
3. 然后优先实现 Phase 3 的大工具结果卸载，因为这是当前上下文污染最明显的问题。
4. Phase 4 的长期记忆结构化放在短期记忆稳定之后推进。

## 当前代码观察

### ContextEngine

- 当前负责消息保存、上下文窗口裁剪和长期记忆读取。
- 需要逐步把长期记忆读取迁移到 MemoryAdapter。
- 参考位置：`src/context_engine/context_engine.cpp:101`、`src/context_engine/context_engine.cpp:263`

### Agent

- 当前直接创建 `HistoryStore` 和 `DreamProcessor`。
- 需要迁移为 MemoryRuntime / MemoryAdapter。
- 参考位置：`src/core/agent.cpp:78`、`src/core/agent.cpp:81`

### ReactWorker

- 当前工具结果完整写入 tool message。
- 需要在写入前增加 payload offload 处理。
- 参考位置：`src/workers/react_worker.cpp:141`、`src/workers/react_worker.cpp:148`

### DreamProcessor

- 当前长期记忆整理固定为两阶段 prompt + 文件编辑。
- 后续迁移为 Memory Runtime 的默认 LongTermConsolidator。
- 参考位置：`src/core/dream_processor.cpp:326`

### HistoryStore

- 当前以 JSONL 保存历史，并维护 dream cursor。
- 后续迁移为 Memory Runtime 的 EventStore。
- 参考位置：`src/core/history_store.cpp:64`

## 决策记录

### 2026-06-07

- 决定将记忆系统设计为可独立交付的 Memory Runtime，而不是 jiuwen-lite 内部专用模块。
- jiuwen-lite 通过 MemoryAdapter 接入。
- Memory Runtime 未来需要支持 C++ SDK、memory-server、HTTP API、MCP server 多种形态。
- 短期记忆重点支持压缩和大 payload 卸载。
- 长期记忆重点支持分层摘要、实体和关系。
- 新增 `include/memory_types.h`，定义 MemoryEvent、MemoryContextRequest、MemoryContextPackage、MemoryPayloadRef、MemoryEntity、MemoryRelation、MemoryStats 等协议类型。
- 新增 `include/memory_runtime.h`，定义 MemoryRuntime 抽象接口。
- 新增 `MemoryConfig` 并接入 `AgentConfig` 和 JSON 配置读写。
- `ResourceManager` 新增 memory runtime provider 注册、创建、查询接口。
- 新增 `include/memory_adapter.h`，定义宿主框架到 Memory Runtime 的适配接口。
- 修复 unittest 框架：测试用例注册阶段不再立即执行测试，改为 `RunAllTests` 阶段统一执行，解决静态初始化阶段卡住导致无输出的问题。
- 为 agent config 单测补充 MemoryConfig JSON 往返断言。
- 已运行 `./build_linux.sh`，构建通过。
- 已运行 `cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，42 个单元测试全部通过。
- 新增 `src/memory/builtin_memory_runtime.*`，提供 `builtin.compat` 兼容实现。
- `builtin.compat` 支持读取旧版 `MEMORY.md`、`SOUL.md`、`USER.md`，并提供安全降级的 AppendEvent、BuildContext、ReadPayload、SearchMemory、GetStats 实现。
- `ResourceManager` 默认注册 `builtin.compat` memory runtime provider。
- 新增 `unittest/test_memory_runtime.cpp`，覆盖 provider 注册、旧 memory 文件读取、事件统计和 file payload 读取。
- 已运行 `cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，46 个单元测试全部通过。
- 已运行 `./build_linux.sh`，构建通过。
- `ContextEngine` 新增可选 memory context provider，未注入或 provider 返回空时回退旧版 `LoadMemoryContext()`。
- `SessionManager` 在 `memoryConfig.enabled=true` 时为 session 创建 `MemoryRuntime`，并将 `BuildContext().memoryText` 注入 `ContextEngine::GetMemoryContent()`。
- 新增 `MemoryContextProviderOverridesLegacyMemory` 单测，覆盖 ContextEngine 使用 runtime provider 的路径。
- 已运行 `cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，47 个单元测试全部通过。
- 已运行 `./build_linux.sh`，构建通过。
- `ContextEngine` 新增可选 memory event sink，`AddMessage` 成功后转发 `MemoryEventType::MESSAGE_APPENDED`。
- `SessionManager` 在 memory runtime 启用时将 event sink 绑定到 `MemoryRuntime::AppendEvent`。
- 新增 `AddMessageEmitsMemoryEvent` 单测，覆盖消息到 MemoryEvent 的转换。
- 已运行 `cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，48 个单元测试全部通过。
- 已运行 `./build_linux.sh`，构建通过。
- `Agent` 在 `memoryConfig.enabled=true` 时创建 agent-level `MemoryRuntime`，用于后台 consolidation。
- `Agent::ConsolidationLoop` 在 MemoryRuntime 可用时调用 `MemoryRuntime::Consolidate`，否则保持现有 `DreamProcessor::Run` 路径。
- 已运行 `cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，48 个单元测试全部通过。
- 已运行 `./build_linux.sh`，构建通过；构建过程中出现 clock skew 警告，但目标产物生成成功。
- 调整 consolidation 兼容语义：`MemoryRuntime::Consolidate` 返回 false 时继续回退执行 `DreamProcessor::Run`，避免启用 memory runtime 后丢失旧 Dream 能力。
- 已运行 `cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，48 个单元测试全部通过。
- 已运行 `./build_linux.sh`，构建通过。
- 调整 MemoryRuntime 生命周期：由 Agent 持有唯一 agent-level runtime，SessionManager 复用该 runtime 绑定 context provider 和 event sink，避免 session runtime 与 consolidation runtime 分裂。
- 移除 SessionEntry 内 per-session MemoryRuntime 持有。
- 已运行 `cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，48 个单元测试全部通过。
- 已运行 `./build_linux.sh`，构建通过。
- `BuiltinMemoryRuntime::AppendEvent` 对 `MESSAGE_APPENDED` 事件追加旧版 `memory/history.jsonl`，并维护 `.cursor`，使 Dream 仍可消费兼容历史流。
- 新增 `AppendEventPersistsLegacyHistoryJsonl` 单测，覆盖 history JSONL、cursor、tool metadata 持久化。
- 已运行 `cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，49 个单元测试全部通过。
- 已运行 `./build_linux.sh`，构建通过。
- memory runtime 启用时，Agent 不再通过旧 `HistoryStore::AppendEntry` 写入 user/final assistant，避免与 MemoryRuntime event persistence 双写同一份 `history.jsonl`。
- memory runtime 未启用时，旧 `HistoryStore` 写入路径保持不变。
- 已运行 `cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，49 个单元测试全部通过。
- 已运行 `./build_linux.sh`，构建通过；构建过程中出现 clock skew 警告，但目标产物生成成功。
- 新增 `LongTermConsolidator` 抽象和 `LegacyDreamConsolidator`，由包装类内部持有并调用 `DreamProcessor`。
- `Agent` 改为依赖 `LongTermConsolidator` 抽象，不再直接持有具体 `DreamProcessor`。
- 已运行 `cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，49 个单元测试全部通过。
- 已运行 `./build_linux.sh`，构建通过。
- `LegacyDreamConsolidator` 内部实现改为 `std::unique_ptr` 持有，移除手动 delete。
- 已运行 `cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，49 个单元测试全部通过。
- 已运行 `./build_linux.sh`，构建通过。
- 新增 MemoryRuntime payload offload API：`WritePayload`、`MemoryPayloadWriteRequest`、`MemoryPayloadWriteResult`。
- `BuiltinMemoryRuntime` 在 `enablePayloadOffload=true` 且内容超过阈值时，将 payload 写入 `memory_runtime/payloads` 并返回 `file://` ref 和摘要替代内容。
- `ReactWorker` 在工具结果写入 tool message 前调用 `MemoryRuntime::WritePayload`，默认关闭时保持原始工具结果写入行为。
- 新增 `WritePayloadOffloadsLargeContent` 和 `WritePayloadKeepsSmallContent` 单测。
- 已运行 `cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，51 个单元测试全部通过。
- 已运行 `./build_linux.sh`，构建通过。
- `Message` 新增 `payloadRef` 字段，offload 后的 tool message 会保留 payload 引用。
- `ContextEngine` 将 `payloadRef` 传播到 `MemoryEvent`，并在文本上下文中展示 `payload_ref`。
- `JsonStorage` 和 `DbStorage` 支持持久化与读取 `payloadRef`。
- 新增 `json_storage.PayloadRef` 单测。
- 已运行 `cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，52 个单元测试全部通过。
- 首次运行 `./build_linux.sh` 时因 `dist/linux/jiuwenClaw` 被进程占用导致打包失败；用户手动结束占用进程后重跑成功。
- 新增 `memory_read_payload` session tool，通过 `MemoryRuntime::ReadPayload` 读取已卸载的完整工具结果。
- `ToolBuildContext` 增加 `memoryRuntime` 字段，AgentWorker 在构建 session tool 时传入。
- `ResourceManager` 注册 `memory_read_payload` 工具。
- 验证通过：`cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，53/53 通过；`./build_linux.sh` 通过。
- `BuiltinMemoryRuntime` 记录 offload 产生的 `MemoryPayloadRef` 元数据。
- `BuildContext` 增加 `## Offloaded Payloads` 概览，并返回 `payloadRefs`，便于模型知道哪些冷数据可通过 `memory_read_payload` 拉回。
- 新增 `BuildContextIncludesOffloadedPayloadOverview` 单测。
- 验证通过：`cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，54/54 通过；`./build_linux.sh` 通过。
- 新增 `MemorySqliteStore`，初始化 `memory_events`、`memory_payloads`、`memory_summaries`、`memory_entities`、`memory_relations` 基础 schema。
- `BuiltinMemoryRuntime` 创建 `memory_runtime/memory.db`，并同步写入 message events 与 payload refs。
- `MemoryStats` 改为优先从 SQLite 汇总 events/payloads/summaries/entities/relations 数量。
- 新增 `PersistsEventsAndPayloadsToSqliteStats` 单测。
- 验证通过：`cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，55/55 通过；`./build_linux.sh` 通过。
- `MemorySqliteStore` 新增 `SaveSummary`、`SaveEntity`、`SaveRelation`，支持长期记忆摘要、实体和关系写入。
- 新增 `MemorySqliteStorePersistsSummaryEntityRelation` 单测。
- 验证通过：`cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，56/56 通过；`./build_linux.sh` 通过，构建中有 clock skew 警告但产物生成成功。
- `MemorySqliteStore` 新增 `LoadLongTermMemoryText`，读取 summaries/entities/relations 并渲染为长期记忆文本。
- `BuiltinMemoryRuntime::BuildContext` 合并 SQLite 长期记忆文本到 `memoryText`。
- 新增 `BuildContextIncludesStructuredLongTermMemory` 单测。
- 验证通过：`cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，57/57 通过；`./build_linux.sh` 通过，构建中有 clock skew 警告但产物生成成功。
- `MemorySqliteStore` 的 `SaveSummary`、`SaveEntity`、`SaveRelation` 支持写入 `sourceRefs` JSON 数组。
- `LoadLongTermMemoryText` 读取并展示 source refs（非空时显示 `sources=[...]`）。
- `BuildContextIncludesStructuredLongTermMemory` 单测更新为验证 source refs 输出。
- 验证通过：`cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，57/57 通过；`./build_linux.sh` 通过。
- `MemoryRuntime::Consolidate` 实现非 LLM session summary：合并最近 events 写入 SQLite `memory_summaries`，source refs 指向 session。
- 新增 `ConsolidateWritesSessionSummary` 单测，验证从 AppendEvent → Consolidate → BuildContext 的完整闭环。
- 验证通过：`cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，58/58 通过；`./build_linux.sh` 通过。
- `memory-server` 独立服务实现，使用 cpp-httplib 提供 REST API：
  - `POST /v1/events` - 写入事件
  - `POST /v1/context` - 构建上下文
  - `GET /v1/payloads/{ref}` - 读取卸载内容
  - `POST /v1/consolidate` - 触发 consolidation
  - `POST /v1/search` - 搜索记忆
  - `GET /v1/stats` - 统计信息
  - `GET /health` - 健康检查
- 构建通过：`cmake --build build-linux --target memory-server -j 16` 通过。
- HTTP smoke test 通过：启动 `memory-server` 后验证 `/health`、`POST /v1/events`、`POST /v1/consolidate`、`POST /v1/context`、`GET /v1/stats`。
- 在 `mem-system-architecture.md` 中补充 MCP server 工具 schema：`memory_append_event`、`memory_build_context`、`memory_read_payload`、`memory_consolidate`、`memory_search`、`memory_stats`。
- 实现 `memory-mcp-server`：JSON-RPC over stdio，支持 initialize、tools/list、tools/call，6 个 memory 工具。
- MCP smoke test 通过：initialize → tools/list → memory_append_event → memory_consolidate → memory_stats。
- 在 `mem-system-architecture.md` 中补充外部框架接入章节：Generic REST Adapter 接入流程、事件映射建议、上下文注入建议、降级策略、openclaw sidecar 模式、Hermes MCP 模式。
- 新增 `LLMLongTermMemoryProcessor`，复用 `Model::Format(systemPrompt, messages, tools)` 与 `Model::Invoke(formattedInput, onChunk)` 做长期记忆 JSON 提取。
- `MemoryRuntime` 新增带 `Model*` 的 `Consolidate` 重载，旧接口保持兼容；`BuiltinMemoryRuntime` 在有模型时优先使用 LLM processor，无有效 LLM 输出时回退规则 processor。
- `Agent::ConsolidationLoop` 在 MemoryRuntime 路径中创建模型并传入 consolidation，使后台长期记忆具备 LLM extraction 能力。
- 新增 `ConsolidateUsesLlmProcessorWhenModelProvided` 与 `ConsolidateFallsBackWhenLlmJsonInvalid` 单测，覆盖 LLM JSON 提取和无效 JSON fallback。
- 验证通过：`cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，61/61 通过；`./build_linux.sh` 通过。
- 实现长期记忆 conflict / supersede / obsolete 处理：`memory_entities` 增加 `active`、`superseded_by`，`memory_relations` 增加 `active`。
- `SaveRelation` 写入 `supersedes` 关系时会将被替代实体标记为 obsolete；相同 `(from_entity, relation)` 的旧关系会被标记 inactive，BuildContext 只加载 active 实体和关系。
- `SaveEntity` 支持同 ID 更新，并在 metadata 中保留 previous summary；支持通过 entity metadata 标记 obsolete/supersedes。
- `MemorySqliteStore::Initialize` 支持对旧数据库补齐新增列。
- 新增 `SupersedesRelationMarksOldEntityObsolete`、`EntityUpdatePreservesPreviousSummary`、`RelationUpdateMarksOldInactive`、`SchemaMigrationAddsActiveColumns` 单测。
- 验证通过：`cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，65/65 通过；`./build_linux.sh` 通过。
- 关于服务端 LLM 能力，决策为：在 memory-server 配置中增加模型配置，server 启动后持有模型；API 调用方不需要传模型。
- `memory-server` 支持 `--model-config <path>`，启动时创建并持有模型；`POST /v1/consolidate` 自动调用 `Consolidate(request, model)`，未配置模型时回退规则路径。
- `memory-mcp-server` 支持 `--model-config <path>`，`memory_consolidate` 自动使用 server 持有的模型。
- HTTP smoke test 通过：`/health`、`POST /v1/events`、`POST /v1/consolidate`。
- MCP smoke test 通过：`initialize`、`memory_append_event`、`memory_consolidate`。
- 验证通过：`cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，65/65 通过；`./build_linux.sh` 通过，构建中有 clock skew 警告但产物生成成功。
- 新增 `examples/memory_server/model_config.example.json`，提供 server LLM 配置样例，敏感字段使用占位符。
- 新增本地 `model_config.local.json`，用于用户填写真实模型配置；已通过 `.gitignore` 忽略。
- `.gitignore` 增加 `model_config.local.json`，避免模型 API key 误提交。
- `mem-system-architecture.md` 补充 memory-server 启动命令、配置文件复制方式、最小 HTTP 调用示例和跨框架接入说明。
- 验证通过：`cmake --build build-linux --target unittest -j 16 && ./build-linux/unittest`，65/65 通过；`./build_linux.sh` 通过，构建中有 clock skew 警告但产物生成成功。
- 决定 Memory Runtime 拆分为独立仓库/包，提供 C++ SDK、HTTP server、MCP server 多种 API 接入方式。
- jiuwen-lite 默认使用 SDK 模式接入：`memoryConfig.mode = "sdk"`。
- jiuwen-lite 支持 server 模式接入：`memoryConfig.mode = "server"`，配置 `serverUrl`、`serverApiKey`、`serverTimeoutSeconds` 后通过 `HttpMemoryRuntime` 调用 memory-server。
- `MemoryConfig` 新增 `mode`、`serverUrl`、`serverApiKey`、`serverTimeoutSeconds`。
- 新增 `HttpMemoryRuntime`，支持 `AppendEvent`、`BuildContext`、`WritePayload`、`ReadPayload`、`Consolidate`、`SearchMemory`、`GetStats` 通过 HTTP 调用 memory-server。
- `memory-server` 新增 `POST /v1/payloads`，支持 server 模式下从 jiuwen-lite 写入大 payload。
- 新增 `examples/memory_server/agents_server_mode.example.json`，按 `data/agents.json` 风格提供 server 模式配置样例。
- 新增 `ResourceManagerRegistersHttpServerRuntime` 单测。
- 验证通过：`cmake --build build-linux --target unittest memory-server memory-mcp-server -j 16 && ./build-linux/unittest`，66/66 通过；`./build_linux.sh` 通过；HTTP payload smoke test 通过。
- Phase 7 独立仓交付准备完成：在 `mem-system-architecture.md` 补充独立仓目录结构、jiuwen-lite 内当前对应文件、memory-only 产物布局、最小依赖边界、target 拆分建议和 SDK/HTTP/MCP README 草案。
- 新增 `build_memory_runtime_linux.sh`，用于构建并打包 memory-server、memory-mcp-server、agent framework 库、公共头文件和 memory_server 示例到 `dist/memory-runtime/linux`。
- 新增 `doc/memory_runtime_readme.md`，作为未来独立仓 README 草案，覆盖 SDK 模式、HTTP sidecar 模式、MCP server 模式、jiuwen-lite server 模式配置和最小依赖边界。

## 中断恢复提示

下次继续实施时，建议从以下步骤开始：

1. 阅读 `mem-system-vision.md` 明确目标边界。
2. 阅读 `mem-system-architecture.md` 确认模块拆分。
3. 从本文件的 Phase 1 待办开始，先设计 memory types 和接口。
4. 实施前优先检查当前 `AgentConfig`、`ResourceManager`、`ContextEngine` 的接口变化。
5. 每完成一个小阶段，更新本文件对应 checkbox 和决策记录。

# 配置系统设计文档

## 1. 模块概述

配置系统（`include/types.h`、`include/config_node.h`、`include/config/`、`src/config/`）是框架的配置基础设施，涵盖从类型定义到 JSON 序列化、持久化存储和热加载监视的完整配置生命周期。

模块包含以下组件：

| 类/结构 | 文件 | 职责 |
|---------|------|------|
| `AgentConfig` | `include/types.h` | 根配置结构 |
| `ConfigNode` | `include/config_node.h` | 变体树配置节点 |
| `AgentConfigJson` | `include/config/agent_config_json.h` | JSON 序列化/反序列化 |
| `AgentConfigStore` | `include/config/agent_config_store.h` | 持久化配置存储 |
| `ConfigWatcher` | `include/config/config_watcher.h` | 文件变更轮询监视 |

## 2. AgentConfig 根配置结构

### 2.1 结构定义

`AgentConfig` 是框架的根配置结构，所有配置项均由此结构派生：

```
AgentConfig
 ├── id (string)                    ← Agent 唯一标识
 ├── name (string)                  ← Agent 名称
 ├── description (string)           ← Agent 描述
 ├── version (string)               ← 版本号
 ├── mode (AgentWorkMode)           ← 工作模式 (REACT / PLAN_AND_EXECUTE / WORKFLOW)
 │
 ├── modelConfig (ModelConfig)      ← 模型配置
 │   ├── baseUrl, apiKey, modelName
 │   ├── provider, formatType
 │   ├── useNativeFunctionCalling
 │   └── extraParams (ConfigNode)
 │
 ├── promptTemplates (map<string, PromptResource>) ← 提示词模板
 │   ├── "system" → PromptResource{type, value}
 │   ├── "react_system" → PromptResource{...}
 │   └── "dream_phase1" → PromptResource{...}
 │
 ├── contextConfig (ContextConfig)  ← 上下文配置
 │   ├── maxContextTokens, maxMessages
 │   ├── sessionId, storagePath, storageType
 │   └── enableSummarization
 │
 ├── dreamConfig (DreamConfig)     ← Dream 整合配置
 │   ├── dataBasePath, historyPath
 │   ├── maxBatchSize, maxIterations
 │   └── 各种字符限制参数
 │
 ├── memoryConfig (MemoryConfig)   ← 记忆配置
 │   ├── enabled, mode, provider
 │   ├── dataPath, serverUrl
 │   ├── offloadToolResultChars, enablePayloadOffload
 │   ├── idleConsolidationSeconds              ← 整合轮询间隔（原 contextConfig，已迁移）
 │   ├── excludedConsolidationSessionIds       ← 整合排除集（如 __CRON__/__HEARTBEAT__）
 │   ├── modelEnabled, modelBaseUrl, ...
 │   └── extraParams
 │
 ├── skillDirectory (string)       ← 技能目录路径
 ├── maxIterations (int)           ← ReAct 循环最大迭代数 (默认 10)
 │
 ├── dataBasePath (string)         ← 数据根目录 (默认 "./data")
 ├── maxConcurrentSessions (int)   ← 最大并发会话数 (默认 3)
 ├── defaultTools (vector<string>) ← 默认可用工具列表
 ├── mcpServerIds (vector<string>) ← MCP 服务器 ID 列表
```

### 2.2 子配置结构

**ModelConfig** — 模型配置

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| baseUrl | string | - | API 端点 |
| apiKey | string | - | 认证密钥 |
| modelName | string | - | 模型名称 |
| provider | string | "" | 自定义 provider |
| formatType | ModelFormatType | OPENAI | API 协议格式 |
| useNativeFunctionCalling | bool | true | 原生 function calling |
| extraParams | ConfigNode | - | 扩展参数 |

**ContextConfig** — 上下文配置

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| maxContextTokens | int | 4096 | 最大 token 数 |
| maxMessages | int | 50 | 最大消息数 |
| sessionId | string | - | 会话 ID |
| storagePath | string | - | 存储路径 |
| storageType | StorageType | JSON_FILE | 存储类型 |
| enableSummarization | bool | false | 启用压缩 |

> `idleConsolidationSeconds` 原在此处，已迁移至 `MemoryConfig`（与整合策略字段集中）。旧配置文件中的 `contextConfig.idleConsolidationSeconds` 仍能被反序列化双读 fallback 拾起，但写回时只写新位置。

**DreamConfig** — Dream 整合配置

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| dataBasePath | string | - | 数据路径 |
| historyPath | string | - | 历史路径 |
| maxBatchSize | int | 20 | 每次最大历史条目数 |
| maxIterations | int | 10 | Phase 2 最大迭代数 |
| maxToolResultChars | int | 16000 | 工具结果截断 |
| historyEntryPreviewMaxChars | int | 4000 | 历史条目预览截断 |
| memoryFileMaxChars | int | 32000 | 记忆文件截断 |

**MemoryConfig** — 记忆配置（与上下文引擎解耦的记忆子系统策略）

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| enabled | bool | true | 是否启用记忆子系统 |
| mode | string | "sdk" | 运行模式（`sdk` / `server`） |
| provider | string | "builtin.compat" | runtime provider |
| dataPath | string | - | 数据目录 |
| serverUrl | string | - | HTTP server 模式下的远端 URL |
| serverApiKey | string | - | HTTP server 鉴权密钥 |
| serverTimeoutSeconds | int | 10 | HTTP 调用超时 |
| serverMaxRetries | int | 2 | HTTP 瞬态错误重试次数 |
| serverCircuitThreshold | int | 5 | 熔断阈值 |
| serverCircuitCooldownSeconds | int | 30 | 熔断冷却时间 |
| tokenBudget | int | 4096 | BuildContext token 预算 |
| offloadToolResultChars | int | 8000 | 工具结果 offload 阈值 |
| enablePayloadOffload | bool | true | 是否启用 offload |
| idleConsolidationSeconds | int | 60 | 整合轮询间隔（原 `ContextConfig`，已迁移至此） |
| excludedConsolidationSessionIds | vector\<string\> | {} | 整合排除集：被排除 session 的事件仍入库、仍推进 cursor，但不进入 batch、不触发 hasNewActivity_。应用层通常填入系统机械触发会话（如 `__CRON__`/`__HEARTBEAT__`） |
| modelEnabled | bool | false | runtime 自带模型开关 |
| modelFormatType | string | "openai" | runtime 模型协议格式 |
| modelBaseUrl | string | - | runtime 模型端点 |
| modelApiKey | string | - | runtime 模型密钥 |
| modelName | string | - | runtime 模型名 |
| modelOrganization | string | - | runtime 组织 |
| modelAnthropicVersion | string | "2023-06-01" | Anthropic 协议版本 |
| modelTimeoutSeconds | int | 60 | runtime 模型超时 |
| modelTemperature | double | 0.0 | runtime 模型温度 |
| modelMaxTokens | int | 0 | runtime 模型 max tokens |
| extraParams | ConfigNode | - | 扩展参数 |

> `idleConsolidationSeconds` 与 `excludedConsolidationSessionIds` 共同驱动 `Agent::ConsolidationLoop`：前者控制轮询间隔，后者控制哪些 session 的事件进入整合批次（同时控制 `NotifySessionIdle` 是否唤醒脏标记）。两者均为 Agent 级配置，应用层在启动时填入自己的系统会话标识。

**McpServerConfig** — MCP 服务器配置

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| id | string | - | 服务器 ID |
| name | string | - | 服务器名称 |
| description | string | - | 描述 |
| enabled | bool | true | 是否启用 |
| type | string | - | 传输类型 ("streamable-http-client", "stdio", "sse") |
| url | string | - | HTTP 服务器 URL |
| command | string | - | STDIO 启动命令 |
| args | vector<string> | - | 命令参数 |
| env | map<string,string> | - | 环境变量 |
| headers | map<string,string> | - | 自定义请求头 |

## 3. ConfigNode 变体树

### 3.1 设计意图

`ConfigNode` 是一个类型安全的配置变体树，替代 `std::any`，用于存储模型扩展参数和其他层级配置。

### 3.2 类型定义

```cpp
using ConfigValue = std::variant<
    int,
    float,
    bool,
    std::string,
    std::vector<std::string>,
    std::shared_ptr<ConfigNode>     // 递归指针支持嵌套层级
>;

struct ConfigNode {
    std::map<std::string, ConfigValue> fields_;

    void Set(const std::string& key, ConfigValue value);
    void SetNested(const std::string& path, ConfigValue value);  // 点号路径
    const T* GetPtr(const std::string& key) const;
    T GetValue(const std::string& key, T defaultVal) const;
};
```

### 3.3 点号路径支持

`SetNested` 支持点号分隔的层级路径：

```cpp
config.extraParams.SetNested("model.temperature", 0.7f);
```

自动创建嵌套节点：

```
ConfigNode {
  "model" → ConfigNode {
    "temperature" → 0.7f
  }
}
```

### 3.4 类型安全访问

```cpp
// 安全访问（返回指针，类型不匹配返回 nullptr）
const float* temp = config.extraParams.GetPtr<float>("model.temperature");

// 带默认值的访问
float temp = config.extraParams.GetValue<float>("model.temperature", 0.0f);
```

相比 `std::any`，`ConfigValue` 的优势：
- `std::variant` 提供静态类型安全
- 编译期检查所有可能的类型
- 不需要 `std::any_cast` 的运行期类型检查
- 嵌套通过 `shared_ptr<ConfigNode>` 递归，不依赖 `std::any`

## 4. AgentConfigJson 序列化

### 4.1 核心函数

```cpp
nlohmann::json AgentConfigToJson(const AgentConfig& cfg);
void MergeAgentConfigFromJson(const nlohmann::json& j, AgentConfig& out);
AgentConfig MergeAgentConfig(const AgentConfig& base, const nlohmann::json& overrideJson);
```

### 4.2 序列化规则

- 所有字段序列化为 JSON 对象
- 枚举值转换为字符串（`WorkModeToString`、`FormatTypeToString`、`StorageTypeToString`）
- `PromptResource` 根据 `type` 字段区分 TEXT 和 FILE_PATH
- `ConfigNode` 通过 `ConfigNodeToJson` 递归序列化
- 未知字段在解析时忽略

### 4.3 默认 + 覆盖合并

`MergeAgentConfigFromJson` 是"默认 + 覆盖合并"模式的基础：

```
// 代码中的默认配置
AgentConfig defaultCfg = BuildAgentConfig();

// JSON 中的用户覆盖
nlohmann::json overrideJson = { "name": "My Agent", "maxIterations": 15 };

// 合并: JSON 中存在的字段覆盖默认值，不存在的保留默认
MergeAgentConfigFromJson(overrideJson, defaultCfg);
```

### 4.4 辅助函数

```cpp
// 枚举转换
string WorkModeToString(AgentWorkMode mode);    // REACT → "react"
bool WorkModeFromString(const string& s, AgentWorkMode& out);  // "react" → REACT

string FormatTypeToString(ModelFormatType t);    // OPENAI → "openai"
bool FormatTypeFromString(const string& s, ModelFormatType& out);

string StorageTypeToString(ContextConfig::StorageType t);  // JSON_FILE → "json_file"
bool StorageTypeFromString(const string& s, ContextConfig::StorageType& out);

// ConfigNode 转换
nlohmann::json ConfigNodeToJson(const ConfigNode& node);
void ConfigNodeFromJson(const nlohmann::json& j, ConfigNode& out);
```

## 5. AgentConfigStore 持久化配置存储

### 5.1 设计意图

`AgentConfigStore` 管理 Agent 配置的持久化存储，支持"代码默认 + JSON 覆盖"的合并模式，并为未来的多 Agent 部署预留了 schema。

### 5.2 类结构

```
AgentConfigStore (Meyers 单例)
  │  ├── mutex_ (mutex)
  │  ├── path_ (filesystem::path)     ← "./data/agents.json"
  │  ├── defaults_ (map<string, AgentConfig>) ← 注册的代码默认配置
  │  ├── current_ (map<string, AgentConfig>) ← 合并后的有效配置
  │  ├── overrides_ (map<string, json>) ← 原始 JSON 覆盖（仅用户提供的字段）
  │  │
  │  ├── SetPersistPath(path)         ← 设置存储路径
  │  ├── RegisterDefault(def)         ← 注册代码默认
  │  ├── Load()                       ← 加载并合并
  │  ├── Get(id) → optional<Config>   ← 查询有效配置
  │  ├── List() → vector<Config>      ← 列出所有配置
  │  ├── Upsert(cfg)                  ← 更新/插入完整配置
  │  ├── UpsertOverride(id, json)     ← 更新/插入覆盖（仅用户字段）
  │  ├── Remove(id)                   ← 移除配置
  │  ├── Save()                       ← 持久化到磁盘
  │  ├── LastWriteTime()              ← 文件修改时间（用于 watcher）
  │  └── FileExists()                 ← 文件是否存在
```

### 5.3 持久化 Schema

`agents.json` 使用多 Agent schema（但当前只运行一个 Agent）：

```json
{
  "version": 1,
  "agents": [
    {
      "id": "default-agent",
      "name": "My Agent",
      "maxIterations": 15,
      "modelConfig": {
        "baseUrl": "http://localhost:8080/v1",
        "modelName": "gpt-4o"
      }
    }
  ]
}
```

### 5.4 默认 + 覆盖合并流程

```
RegisterDefault(BuildAgentConfig())
  │  ├── defaults_["default-agent"] = BuildAgentConfig() 的结果
  │  │   ← 包含所有代码硬编码的默认值
  │
  ▼
Load()
  │  ├── 读取 agents.json → 解析为 vector<json> overrides
  │  ├── 对每个 id:
  │  │   ├── 若 id 在 defaults_ 中:
  │  │   │   ├── 复制 defaults_[id] → effectiveCfg
  │  │   │   ├── MergeAgentConfigFromJson(override, effectiveCfg)
  │  │   │   │   ← JSON 中存在的字段覆盖默认，不存在的保留
  │  │   │   └── current_[id] = effectiveCfg
  │  │   └── 若 id 不在 defaults_ 中:
  │  │   │   ├── 从 JSON 构建完整 AgentConfig
  │  │   │   └── current_[id] = config (仅 JSON 提供的字段)
  │  └── overrides_ = 保存原始 JSON（用于 Save 时只写用户字段）
  │  └── 返回 current_ map
```

### 5.5 覆盖追踪

`overrides_` 独立追踪用户提供的 JSON 覆盖，确保 `Save()` 只写入用户提供的字段，而不是完全合并后的配置：

```
UpsertOverride("default-agent", {"maxIterations": 15})
  │  ├── overrides_["default-agent"] = {"maxIterations": 15}
  │  ├── 合并到 current_["default-agent"]
  │  └── Save() → 只写入 {"maxIterations": 15}，不写入代码默认值
```

如果不单独追踪覆盖，`Save()` 会将所有代码默认值也写入 `agents.json`，导致：
- 默认值被固化到文件中
- 代码更新默认值时，文件中的旧默认值不会被更新
- 代码默认与文件默认冲突

### 5.6 Upsert vs UpsertOverride

| 方法 | 输入 | 用途 | Save 行为 |
|------|------|------|----------|
| `Upsert(cfg)` | 完整 AgentConfig | Web UI 保存完整配置 | 序列化所有字段到 JSON |
| `UpsertOverride(id, json)` | 原始 JSON | 保存仅用户编辑的字段 | 只写入用户提供的字段 |

`UpsertOverride` 更适合 API/Web UI 场景，因为它不会将代码默认值固化到磁盘。

## 6. ConfigWatcher 文件变更监视

### 6.1 设计意图

`ConfigWatcher` 轮询监视一组文件的修改时间变化，当文件被修改时触发回调。用于实现配置的热加载（live reload）。

### 6.2 类结构

```
ConfigWatcher
  │  ├── mutex_ (mutex)
  │  ├── cv_ (condition_variable)
  │  ├── running_ (atomic<bool>)
  │  ├── pollSeconds_ (int)           ← 轮询间隔（默认 3 秒）
  │  ├── thread_ (thread)            ← 监视线程
  │  ├── entries_ (vector<Entry>)    ← 监视条目
  │  │
  │  ├── Watch(path, callback)       ← 注册监视条目
  │  ├── Start(pollSeconds)          ← 启动监视线程
  │  ├── Stop()                      ← 停止监视线程
  │  ├── Poke()                      ← 立即唤醒（强制检查）
  │  └── Loop()                      ← 线程主循环
  │
  │  Entry {
  │    path (string)                  ← 文件路径
  │    cb (Callback)                  ← 变更回调
  │    lastMtime (file_time_type)    ← 上次修改时间
  │  }
```

### 6.3 工作流程

```
Loop()
  │  ├── while (running_)
  │  │   ├── cv_.wait_for(pollSeconds_)
  │  │   ├── 对每个 Entry:
  │  │   │   ├── 读取当前 mtime
  │  │   │   ├── 若 mtime != lastMtime → cb(path) + 更新 lastMtime
  │  │   │   └── 若 mtime == lastMtime → 无操作
  │  │   └── 继续循环
  │  └── running_ == false → 退出
```

### 6.4 Poke 机制

`Poke()` 立即唤醒监视线程（通过通知条件变量），用于显式触发检查：

- 用户执行 `/reload` 命令后立即检查配置变更
- 不需要等待下一个轮询周期

### 6.5 应用层集成

jiuwenClaw 使用 `ConfigWatcher` 监视三个配置文件：

```cpp
// main.cpp
ConfigWatcher watcher;

watcher.Watch("./data/agents.json", [&](path) {
    // Agent 配置变更 → ReloadAgent
    auto configs = store.Load();
    auto cfg = configs[config_.id];
    SessionManager::GetSessionManager().ReloadAgent(cfg);
});

watcher.Watch("./data/channels.json", [&](path) {
    // 通道配置变更 → ReloadChannels
    ReloadChannels();
});

watcher.Watch("./data/mcp_servers.json", [&](path) {
    // MCP 配置变更 → ReloadMcpServers
    ReloadMcpServers();
});

watcher.Start(3);  // 每 3 秒轮询
```

### 6.6 mtime 初始值

Entry 的 `lastMtime` 初始值设为 `file_time_type::min()`：

- 首次检查时，任何存在的文件的 mtime 都大于 `min()`
- 确保首次启动时立即触发回调（加载当前配置）

Windows 平台下，`min()` 可能被 `<windows.h>` 的 `min/max` 宏展开，使用括号 `(min)()` 防止。

## 7. 配置生命周期总结

### 7.1 完整流程

```
1. 代码默认
   BuildAgentConfig() → 构建硬编码的默认 AgentConfig
   RegisterDefault(config) → 注册到 AgentConfigStore

2. JSON 覆盖加载
   Load() → 读取 agents.json → 合并到代码默认
   → 生成 effective AgentConfig

3. 初始化
   InitSessionManager(effectiveConfig) → 构建 Agent
   → Agent 使用 effectiveConfig 运行

4. 热加载
   ConfigWatcher 检测 agents.json 变更
   → Load() → ReloadAgent(newConfig)
   → Agent 原子替换，会话保留

5. 运行时更新
   UpsertOverride(id, json) → 更新覆盖
   → Save() → 写入磁盘
   → ConfigWatcher 触发 → Load → ReloadAgent

6. 查询
   Get(id) → 返回当前有效配置
   List() → 返回所有 Agent 配置
```

### 7.2 设计要点

- **默认值永不固化**：代码默认在内存中，不写入 `agents.json`
- **覆盖精确追踪**：`overrides_` 独立保存原始 JSON，确保 `Save()` 只写用户字段
- **合并不丢失**：`MergeAgentConfigFromJson` 保留未覆盖字段的代码默认值
- **新字段向前兼容**：代码新增配置字段时，旧 JSON 文件不包含该字段，合并后使用新默认值

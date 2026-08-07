# 工具系统设计文档

## 1. 模块概述

工具系统（`include/tool.h`、`src/tools/`）是 Agent 框架的执行层，提供 Agent 与外部世界交互的能力。工具系统包含：

- `Tool` 抽象基类 — 所有工具的统一接口
- 12 个无状态内置工具 — 独立于会话的通用能力
- 6 个会话级工具 — 依赖会话上下文的交互能力
- `CapabilitySelector` — V2 LLM-backed 能力召回（替代废弃的 `ToolSelector`，按 name 经 `ResourceManager`/`SkillEngine` 取 desc，复用主 `modelConfig` 调 LLM 选 top-K）
- `MCPTool` — MCP 服务器工具的本地代理

## 2. Tool 抽象基类

### 2.1 类定义

```cpp
class Tool {
public:
    Tool(std::string name, std::string description, std::vector<ToolParam> params);
    virtual ~Tool() = default;

    virtual std::string Invoke(const std::string& input) = 0;

    std::string GetName() const;
    std::string GetDescription() const;
    std::vector<ToolParam> GetParams() const;
    std::string GetSchema() const;                    // JSON Schema 字符串
    virtual nlohmann::json GetJsonSchema() const;     // 结构化 JSON Schema

protected:
    std::string name_;
    std::string description_;
    std::vector<ToolParam> params_;
};
```

### 2.2 ToolParam 结构

```cpp
struct ToolParam {
    std::string name;          // 参数名
    std::string description;   // 参数描述
    std::string type;          // 类型（"string", "number", "boolean", "array" 等）
    bool required{false};      // 是否必需
};
```

### 2.3 Schema 自动构建

`GetJsonSchema()` 默认从 `ToolParam` 列表自动构建 JSON Schema：

```json
{
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "File path to read"
    },
    "offset": {
      "type": "number",
      "description": "Line number to start reading from"
    }
  },
  "required": ["path"]
}
```

工具可覆写 `GetJsonSchema()` 以提供更复杂的 Schema（如枚举、嵌套对象、默认值等）。

### 2.4 Command 模式

每个工具本质上是一个**Command 对象**：`Invoke(input)` 执行操作并返回结果字符串。输入是 JSON 编码的参数对象，输出是文本结果。

## 3. 无状态内置工具

### 3.1 工具清单

| 名称 | 类 | 描述 | 关键参数 |
|------|-----|------|---------|
| `time_info` | `TimeInfoTool` | 获取当前日期/时间/星期 | `format`（可选） |
| `web_search` | `WebSearchTool` | 网络搜索（支持引擎回退） | `query`（必需） |
| `web_fetcher` | `WebFetcherTool` | 获取网页内容 | `url`（必需） |
| `read_file` | `ReadFileTool` | 读取文件内容 | `path`（必需）、`offset`/`limit`（可选） |
| `write_file` | `WriteFileTool` | 写入文件 | `path`（必需）、`content`（必需） |
| `edit_file` | `EditFileTool` | 原地编辑文件 | `path`、`old_string`、`new_string`（均必需） |
| `list_dir` | `ListDirTool` | 列出目录内容 | `path`（必需） |
| `glob` | `GlobTool` | 文件路径模式匹配 | `pattern`（必需）、`path`（可选） |
| `grep` | `GrepTool` | 文件内容搜索 | `pattern`（必需）、`path`（可选） |
| `exec` | `ExecTool` | 执行 Shell 命令 | `command`（必需）、`timeout`（可选） |
| `skill_search` | `SkillSearchTool` | 搜索并加载技能指令 | `query`（必需） |
| `file_state` | `FileStateTool` | 跟踪文件状态变化 | `path`（必需） |

### 3.2 文件操作工具族

`read_file`、`write_file`、`edit_file`、`list_dir`、`glob`、`grep`、`file_state` 构成文件操作工具族，赋予 Agent 完整的文件系统交互能力。

`edit_file` 采用**精确字符串替换**模式（类似 sed），而非行号模式，因为 LLM 生成的行号经常不准确。

#### `web_search` 反爬与多后端设计

`web_search` 按**查询语言路由**两层引擎链：

| 查询语言 | 引擎顺序 |
|---|---|
| 含 CJK 字符 | **Baidu（直连国内 IP）→ Sogou（经代理）**——CJK 查询到此停止 |
| 纯 ASCII | DDG → Wikipedia → Bing |

引擎选择由 `ContainsCJK(query)` 决定（UTF-8 解码，检测 U+3000-U+9FFF / U+F900-U+FAFF / U+FF00-U+FFEF CJK Unicode 块）。中文查询走中文引擎：从国内 IP 直连 Baidu（不经代理、不共享代理出口 IP 的封禁问题），中文新闻/财经/趣闻查询覆盖更好；英文查询走 Western 引擎链：Baidu/Sogou 的英文索引覆盖差，跳过。

**Layer 1 — 请求形态伪装**（降低挑战触发率）：

- UA per-process 固定：进程首次调用时从 5 个 Chrome / Firefox / Win / Mac / Linux UA 池中按 `std::random_device` 锁定一个，后续不变。避免"同 IP 多浏览器切换"的可疑指纹。
- DDG 走 POST 表单提交（`req.body = "q=..."`）替代 GET querystring。某些反爬启发式把 POST 当搜索框提交更友好。
- 跨调用 per-engine 冷却：DDG 8s / Bing 15s / Wikipedia 1s / **Baidu 5s / Sogou 8s**。同引擎的下一次调用若距上次不足冷却时长则 sleep 补足，避免模型连续硬刷触发限流窗口。

**Layer 2 — fail-fast 重试策略**：

`ShouldRetry(resp, challengeDetected)` 仅在以下情况重试：
- curl 传输错误（CURLE_*）
- HTTP 429 / 503（真瞬时状态）

显式**不重试**：
- HTTP 202 / 418 / 403（DDG/Bing/Baidu/Sogou"必须解 JS 挑战"硬封禁——重试只是把封禁坐实并升级）
- body-marker 挑战（`anomaly.js` / `captcha` / `CfConfig` / `百度安全验证` / `antispider` 等——需 JS 执行，plain-HTTP 重试无解）

`challengeDetected` 参数保留以维持 API 稳定，但其值不影响结果——重试决策只在 curl 错误和 429/503 时为真。这把单次失败延迟从 ~9s 降到 ~1s，模型快速拿到反馈。

引擎专属挑战检测：

- **DDG**：`IsDDGChallenge` — 状态 202 / 418 / 429 / 503 或 body 含 `/anomaly.js` / `challenge-form` / `duckduckgo.com/anomaly.js`。
- **Bing**：`IsBingChallenge` — 状态 403 / 429 / 503 或 body 含 `CfConfig` / `class="captcha"` / `/challenge/verify` / `BingBot` / `/identity/`。Bing 反爬页常以 HTTP 200 返回 Cloudflare Turnstile 挑战体，body 标记是主信号。
- **Baidu**：`IsBaiduChallenge` — 状态 403 / 429 / 503 或 body 含 `百度安全验证` / `百度验证` / `waptcha` / `safecheck` / `请输入验证码` / `网络不给力` / `/captcha/`。Baidu 的软挑战页常以 HTTP 200 返回 `<title>百度安全验证</title>` 或"网络不给力，请稍后重试"页面。
- **Sogou**：`IsSogouChallenge` — 状态 403 / 429 / 503 或 body 含 `antispider` / `用户您好` / `请输入验证码` / `/captcha/` / `sogou_vr_captcha`。

**Layer 3'' — 中文搜索引擎后端**：

- **Baidu（直连）**：`https://www.baidu.com/s?wd=<enc>&ie=utf-8&tn=baidurt&rn=<n>`。`noProxyHosts="baidu.com"` 通过 `CURLOPT_NOPROXY` 绕过 `HTTPS_PROXY` 环境变量，让请求从国内 IP 直连——绕开代理出口 IP 的封禁问题（代理出口 IP 被其他 VPN 用户拖累，被西方引擎按 IP 封禁）。`tn=baidurt` 强制返回旧版 HTML 模板（标题在 `<h3 class="t"><a href="external-url">title</a></h3>` 中可见，新版默认模板把标题 JS 渲染到空 `<a>` 标签里，plain-HTML regex 抓不到）。`ie=utf-8` 让 Baidu 返 UTF-8 而非 GBK。结果 URL 是**直接外部 URL**（不经 `baidu.com/link?url=` 重定向跳转），无需解码。snippet 在 JS-molecule 中无法 plain-HTML 提取——agent 用 `web_fetcher` 取详情。
- **Sogou（经代理）**：`https://www.sogou.com/web?query=<enc>&ie=utf8&oe=utf8&rn=<n>`。经 `HTTPS_PROXY` 环境变量（本地 Clash VPN）走代理。`ie=utf8&oe=utf8` 让 Sogou 返 UTF-8。结果块在 `<div class="vrwrap">` 内，title 在 `<a name="dttl" href="/link?url=...">TITLE</a>`，snippet 在 `<div class="fz-mid space-txt ...">SNIPPET</div>`。`/link?url=` 是 Sogou 内部跟踪跳转，保留原 href（解 base64 真实 URL 是单独行为，超范围）。

**Layer 4 — 5 分钟结果缓存**：

`WebSearchResultCache` 单例（`web_search_tool.h` 声明）以 `"<engine>|<query>"` 为 key 缓存成功结果，TTL 默认 300s。命中即跳过冷却 + 网络往返。失败结果不缓存（避免瞬时 IP-flag 污染后续调用）。线程安全（内部 mutex）。

**Layer 5 — 工具 description 行为约束**：

`WebSearchTool` 构造里的 description 字符串明告诉模型："issue ONE web_search call per turn; for details on a result, use web_fetcher on its URL. Do NOT issue multiple web_search calls with rephrased queries in the same turn — that pattern looks like a bot to search engines and triggers anti-bot challenges that block the agent's IP for everyone." 直击"模型 17s 内 3 次近似 query 硬刷"的反爬触发根因。错误消息尾部也追加 IP-flagged 提示，引导模型退回 `web_fetcher` 或重写 query 而非硬刷。

**纯函数与可测性**：

`IsDDGChallenge` / `IsBingChallenge` / `IsBaiduChallenge` / `IsSogouChallenge` / `ShouldRetry` / `ComputeBackoffMs` / `PickUserAgent` / `ContainsCJK` / `ParseDdgLiteResults` / `ParseBingResults` / `ParseBaiduResults` / `ParseSogouResults` / `ParseWikipediaResults` 全部以自由函数形式声明在 `web_search_tool.h`，便于离线单测。`WebSearchResultCache` 为类形式以便注入短 TTL 测试过期。重试循环 `HttpGetWithRetry` + 直连 helper `HttpGetOnce` / `HttpPostOnce` + per-engine 冷却 `CooldownBefore` 留在 `.cpp` 匿名命名空间内（端到端由 `integration_tests/smoke_web_search.cpp` 覆盖）。

**已知的固有局限**：

所有 HTML 抓取类搜索引擎（DDG / Bing / Brave / Baidu / Sogou）都按 IP 限流，agent 出站 IP（无论是 VPN 代理出口 IP 还是直连国内 IP）在连续多次请求后都会被封禁。本设计的 Layer 1+2+5 大幅降低触发概率，但无法完全消除——频繁使用下仍会偶发"全部引擎失败"。若需"始终拿得到结果"，唯一稳健解是接入账号绑定的搜索 API（如 Brave Search API 免费档，按账号限流不按 IP），作为未来保险。

### 3.4 SkillSearchTool 特殊设计

`SkillSearchTool` 使用**静态 `SkillEngine*` 指针**而非 `ToolBuildContext` 注入：

```cpp
// Agent 初始化时设置
static SkillEngine* skillEnginePtr_ = nullptr;

// Agent::构造
skillSearchTool.setSkillEngine(skillEngine_.get());
```

这是因为 `SkillEngine` 是 Agent 级别（而非会话级别）的资源，所有会话共享同一技能引擎。

## 4. 会话级工具

### 4.1 设计意图

会话级工具依赖会话上下文资源，每次调用时通过 `ToolBuildContext` 注入依赖。这确保同一工具在不同会话中使用不同的数据源。

### 4.2 工具清单

| 名称 | 类 | 注入依赖 | 用途 |
|------|-----|---------|------|
| `todo_create` | `TodoCreateTool` | `SessionTodoList*` | 创建任务列表 |
| `todo_complete` | `TodoCompleteTool` | `SessionTodoList*` | 标记任务完成 |
| `todo_insert` | `TodoInsertTool` | `SessionTodoList*` | 在指定位置插入任务 |
| `todo_remove` | `TodoRemoveTool` | `SessionTodoList*` | 移除任务 |
| `todo_list` | `TodoListTool` | `SessionTodoList*` | 列出当前任务 |
| `ask_user` | `AskUserTool` | `AskUserDispatcher*` + `streamCallback` | 向用户请求澄清 |
| `memory_read_payload` | `MemoryReadPayloadTool` | `MemoryRuntime*` | 读取 offloaded payload |

### 4.3 Todo 工具族

Todo 工具族通过 `SessionTodoList*` 指针操作同一个任务列表实例。每次创建工具时注入当前会话的 `SessionTodoList`：

```cpp
// 注册
rm.RegisterSessionTool("todo_create",
    [](const ToolBuildContext& ctx) -> unique_ptr<Tool> {
        return make_unique<TodoCreateTool>(ctx.todoList);
    });

// 使用
auto ctx = ToolBuildContext{.todoList = workerEnv_->GetOrCreateSessionTodoList(sid), ...};
auto tool = rm.CreateSessionTool("todo_create", ctx);
tool->Invoke(input);  // 操作会话的任务列表
```

### 4.4 AskUserTool

`AskUserTool` 是最复杂的会话级工具，实现了**异步问答**模式。

`AskUserDispatcher` 是会话级资源（随 `SessionEntry` 存活），构造时带上 `sessionId` 和 `AskUserRouter*`。`EmitAskUser` 注册 requestId→sessionId 索引（通过 `AskUserRouter`），使 `SessionManager::ProvideUserResponse(requestId, answer)` 能路由到正确会话，即使 Agent 已被热重载。

```
AskUserTool::Invoke(input)
  │  1. 解析 input → 提取问题内容
  │  2. 生成 requestId
  │  3. askUser_->EmitAskUser(requestId, payload, streamCallback)
  │     ├── 注册 Slot (条件变量 + 可选答案)
  │     ├── AskUserRouter::RegisterAskRequest(requestId, sessionId)
  │     └── 发送 [ASK_USER] 标记到流式回调
  │  4. askUser_->WaitForResponse(requestId, timeout)
  │     ├── 阻塞等待 (条件变量)
  │     ├── 完成或超时后 AskUserRouter::UnregisterAskRequest(requestId)
  │     └── 超时返回 nullopt → 工具返回超时消息
  │  5. 收到 answer → 返回答案字符串
```

应用层回应路由：
```
POST /api/answer → SessionManager::ProvideUserResponse(requestId, answer)
  │  1. 查 askRequestToSession_ 索引 → 得到 sessionId
  │  2. 查 sessions_ → 得到 SessionEntry->askUser
  │  3. SessionEntry->askUser->ProvideResponse(requestId, answer)
```

### 4.5 MemoryReadPayloadTool

配合 Payload Offloading 机制使用：

```
MemoryReadPayloadTool::Invoke(input)
  │  1. 解析 input → 提取 payloadRef (URI)
  │  2. memoryRuntime_->ReadPayload(uri)
  │  3. 返回完整 payload 内容
```

## 5. CapabilitySelector 能力召回

### 5.1 设计意图

`CapabilitySelector`（`src/core/capability_selector.{h,cpp}`）为 V2 渐进披露（round5 §5.4.1）提供 LLM-backed 能力召回，替代被废弃的 `ToolSelector`（其选择方法是 `return 1.0;` 桩、`toolPool_` 只持 name 不持 desc，数据通道双重缺陷）。`findRelevant(rawQuery, sessionContext) -> CapabilitySelection{tools, skills}` 一次调用同时选 tool + skill，复用主 `modelConfig`（不引入 `recallModelConfig`），按 name 经 `ResourceManager::GetToolCatalog` / `SkillEngine::GetSkillCatalog` 取 desc（不攒死副本）。

### 5.2 类结构

```cpp
struct CapabilitySelection {
    std::vector<std::string> tools;   // tool 名列表，种入 activeSet
    std::vector<std::string> skills;  // skill 名列表，种入 skillActiveSet
};

class CapabilitySelector {
public:
    explicit CapabilitySelector(AgentConfig config, SkillEngine* skillEngine = nullptr);
    CapabilitySelection findRelevant(const std::string& rawQuery,
                                      const std::vector<Message>& sessionContext);
private:
    AgentConfig config_;
    SkillEngine* skillEngine_;
    std::string BuildRecallPrompt(const std::string& rawQuery,
                                  const std::vector<Message>& sessionContext) const;
    static CapabilitySelection ParseRecallResponse(const std::string& response);
};
```

### 5.3 调用契约

- **turn 起点**：`ReactAgentWorker::Invoke` 入口在 `SELECTIVE` 模式下调一次 `findRelevant`，结果按 §5.4.1 条 6 降级判定表的 4 行分支种入 active set / skillActiveSet（返空/异常 → `seedActive(pool)` 退化 progressive；返 == 全量 → `seedActive(pool)` 全相关短路；返正常子集 → `seedActiveSubset`；返非空但全在 alwaysOn → 仍 `seedActiveSubset` 不退化）。
- **turn 中途**：`tool_search` / `skill_search` 的 search action 在 `isActiveFullPool()` / `isActiveFullSkillPool()` 返 false 时（即子集 seed 路径）调用 `findRelevant(query, {})`（空 sessionContext，因主 LLM 自写的 search query 已带 context）。
- **失败模式**：JSON 解析失败 / LLM 调用异常 / 返空 → 返空 `CapabilitySelection`，由调用方识别为降级触发条件。

### 5.4 演进路径

V3（skill-hub 量级 >1000）可升级 embedding 预筛（descriptor embedding 注册时算一次缓存，query embedding 每轮算一次，接入外部 embedding 端点）。届时 `CapabilitySelector` 接口不变、后端换。未来还可引入独立 `recallModelConfig`（V2 当前复用主 `modelConfig` 以求最简落地）。

## 6. MCPTool 代理模式

### 6.1 设计意图

`MCPTool` 是 MCP 服务器工具的本地代理，将远程工具封装为 `Tool` 子类，使 Agent 可以像调用本地工具一样调用 MCP 工具。

### 6.2 类定义

```cpp
class MCPTool : public Tool {
public:
    MCPTool(std::string name, std::string description,
            std::vector<ToolParam> params,
            std::shared_ptr<MCPConnection> server);
    std::string Invoke(const std::string& input) override;
private:
    std::shared_ptr<MCPConnection> server_;
};
```

### 6.3 Invoke 流程

```
MCPTool::Invoke(input)
  │  1. 解析 input → nlohmann::json arguments
  │  2. server_->CallTool(name_, arguments)
  │     ├── MCPConnection 路由到 MCPClient
  │     ├── MCPClient 发送 JSON-RPC tools/call 请求
  │     └── 接收并解析结果
  │  3. 将 MCPToolResult 格式化为文本
  │     ├── isError → 返回错误消息
  │     └── content → 返回拼接的内容文本
```

### 6.4 Schema 来源

MCPTool 的 Schema 来自 MCP 服务器的 `tools/list` 响应：

```json
// MCP tools/list 响应
{"tools": [{"name": "weather", "description": "Get weather", "inputSchema": {"type": "object", "properties": {"city": {"type": "string"}}}}]}
```

注册时将 `inputSchema` 转换为 `ToolParam` 列表（简化映射），或直接覆写 `GetJsonSchema()` 返回原始 Schema。

## 7. 工具注册与发现流程

### 7.1 启动时注册

```
ResourceManager 构造
  │  ├── RegisterBuiltinTools()
  │  │   ├── RegisterTool("time_info", TimeInfoTool factory)
  │  │   ├── RegisterTool("read_file", ReadFileTool factory)
  │  │   ├── ... 12 个无状态工具
  │  │   ├── RegisterSessionTool("todo_create", TodoCreateTool factory)
  │  │   ├── ... 6 个会话级工具
  │  │   └── 缓存 Schema
  │  │
  │  └── RegisterBuiltinModels()
```

### 7.2 应用层扩展

jiuwenClaw 在 `main.cpp` 中注册应用级工具：

```cpp
// 注册 jiuwenClaw 特有的工具
rm.RegisterTool("notebook_edit", NotebookEditTool factory);
rm.RegisterSessionTool("cron", CronTool factory);
rm.RegisterSessionTool("notify", NotifyTool factory);
```

### 7.3 MCP 工具发现

```
LoadMCPServers(configs)
  │  ├── 对每个 McpServerConfig:
  │  │   ├── RegisterMCPServer(config)
  │  │   │   ├── MCPConnection::Connect()
  │  │   │   ├── MCPConnection::ListTools() → vector<MCPToolInfo>
  │  │   │   └── 对每个 MCPToolInfo:
  │  │   │       ├── RegisterMcpTool(toolName, factory)
  │  │   │       └── factory 创建 MCPTool(name, desc, params, connection)
```

### 7.4 Agent 工具配置

`AgentConfig::defaultTools` 指定 Agent 可用的工具列表：

```cpp
config.defaultTools = rm.GetAvailableTools();  // 获取所有可用工具名称
// 或精选
config.defaultTools = {"read_file", "write_file", "exec", "web_search"};
```

`Agent::AddTools()` 和 `AgentWorker::AddTools()` 可在运行时动态添加工具。

## 8. 线程安全考虑

- **无状态工具**：每次 `CreateTool` 创建新实例，无并发问题
- **会话级工具**：每次 `CreateSessionTool` 创建新实例，注入当前会话的依赖指针
- **`SessionTodoList*`**：由 `SessionEntry` 拥有（随会话存活、跨热重载保留），通过 WorkerEnv 预缓存访问，使用时由 `invokeMutex` 保证串行
- **`AskUserDispatcher*`**：由 `SessionEntry` 拥有（随会话存活、跨热重载保留），内部有 `slotsMu_`（L6）保护
- **`MemoryRuntime*`**：由 `SessionManager` 拥有，各方法内部有锁保护
- **MCPTool**：持有 `shared_ptr<MCPConnection>`，连接内部有 `callMutex_` 保护并发调用

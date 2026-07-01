# CurlClient 统一 HTTP 传输层 — 设计文档

> 本文档为 `CurlClient` 的完整设计规范，作为后续分 Phase 迁移所有 libcurl 使用点的实施蓝图。

## 1. 设计目标

### 核心目标

将核心库中散落的 10 处 `curl_easy_init()`（ephemeral handle 模式）收归到统一的 `CurlClient` 抽象层，实现：

1. **连接复用** — thread_local 持久化 CURL handle，同一线程内连续 HTTP 请求复用 TCP 连接、TLS session、DNS cache
2. **生命周期管理** — `curl_global_init/cleanup` 由 CurlClient 统一管理，消除多线程并发初始化风险
3. **消除临时 handle** — `UrlEncode/UrlDecode` 使用内部 thread_local handle 而非每次新建
4. **统一接口** — 调用方不再直接接触 `curl_easy_*` API，全部通过 `CurlClient::Post/Get/PostStream/GetStream` 操作

### 非目标

- **连接池**（多 handle 并发）— 当前框架并发模型为每线程串行调用，thread_local 单 handle 已足够
- **HTTP/2 多路复用** — 后续可探索，但不在本轮设计范围
- **重试/熔断** — 保留在调用方（HttpMemoryRuntime、OpenAIModel、AnthropicModel 等），CurlClient 只做传输层
- **SSE 行解析** — 保留在调用方（OpenAI StreamContext、Anthropic AnthropicStreamContext），CurlClient 只传递原始 chunk

## 2. 影响范围分析

### 当前 libcurl 使用点

| 子系统 | `curl_easy_init` 数 | 请求模式 | Streaming | 特殊选项 | 跨区域概率 | 调用频率 |
|--------|---------------------|---------|-----------|---------|-----------|---------|
| HttpMemoryRuntime | 3 | POST/GET | 无 | 无 | 中 | 3-6 次/Invoke cycle |
| OpenAIModel | 1 | POST | SSE 流式 | 无 timeout 设置 | 极高 | 1 次/iteration |
| AnthropicModel | 1 | POST | SSE 流式 | 无 timeout 设置 | 极高 | 1 次/iteration |
| MCPClient | 1 | POST | 无 | 动态 headers、timeout 可配置（默认 3s/10s） | 中 | 1-3 次/工具调用 |
| WebSearchTool | 3 | GET | 无 | FOLLOWLOCATION, SSL_VERIFY=0, User-Agent | 高 | 1-3 次/搜索 |
| WebFetchTool | 1 | GET | 无 | FOLLOWLOCATION, SSL_VERIFY=0, USERAGENT | 高 | 1 次/抓取 |

> **应用层不纳入**：FeishuChannel（`examples/jiuwenClaw/adapters/feishu/`，2 处 curl_easy_init）属于应用层，按架构约定不引用核心库内部头（`src/utils/`）。应用层有自己的 utils 副本（`examples/jiuwenClaw/utils/`），传输实现自洽。CurlClient 作为核心库内部传输实现细节，不进公开 API（`include/`）。故 FeishuChannel 保留自己的 curl 用法，不在本次统一范围内。

### 连接复用收益估算

| 场景 | 每次新连接额外延迟 | Invoke cycle 累计节省 |
|------|-------------------|---------------------|
| 同机 localhost | < 1ms | ~0-6ms |
| 同数据中心 | 1-5ms | ~3-30ms |
| 跨区域（模型 API） | 50-200ms | ~50-200ms/iteration × N iterations |

模型 API 是跨区域概率最高、延迟最敏感的路径。连续 iteration 向同一 API host 发请求时，thread_local handle 的连接缓存可实现跨请求复用。

### UrlEncode 临时 handle

`UrlEncode()` 当前在 HttpMemoryRuntime（ReadPayload 路径）和 WebSearchTool（搜索 URL 编码）中使用 `curl_easy_escape()`，每次新建 ephemeral handle 仅用于字符串转义。CurlClient 的 `UrlEncode/UrlDecode` 将使用内部 thread_local handle，不再新建。

## 3. 接口设计

### 3.1 CurlRequest — 请求参数

```cpp
struct CurlRequest {
    std::string url;
    std::vector<std::string> headers;   // curl_slist 形式
    std::string body;                    // POST body（空 = GET）
    long connectTimeout{0};              // CURLOPT_CONNECTTIMEOUT，0 = 不设
    long requestTimeout{0};              // CURLOPT_TIMEOUT，0 = 不设
    bool followLocation{false};          // CURLOPT_FOLLOWLOCATION
    bool sslVerify{true};                // CURLOPT_SSL_VERIFYPEER / VERIFYHOST
    std::string userAgent;               // CURLOPT_USERAGENT，空 = 不设
};
```

**设计原则**：
- 所有 curl_easy_setopt 选项通过结构体传递，调用方不直接接触 curl API
- `body` 非空时自动设置 CURLOPT_POST + CURLOPT_POSTFIELDS + CURLOPT_POSTFIELDSIZE
- `body` 为空时不设 CURLOPT_POST（默认 GET）
- headers 以 `vector<string>` 提供（如 `"Authorization: Bearer xxx"`），CurlClient 内部构建和释放 curl_slist
- timeout 值 0 表示不设置，curl 使用默认值

### 3.2 CurlResponse — 响应结果

```cpp
struct CurlResponse {
    long statusCode{0};                  // HTTP 状态码
    std::string body;                    // 响应体（非 streaming 模式下完整内容）
    CURLcode curlCode{CURLE_OK};        // curl 传输层错误码
    bool isCurlError{false};            // curlCode != CURLE_OK
    std::string curlErrorStr;           // isCurlError 时填充（curl_easy_strerror），调用方无需再接触 curl API
};
```

**设计原则**：
- statusCode 通过 `curl_easy_getinfo(CURLINFO_RESPONSE_CODE)` 提取
- curl 错误分类（IsRetryableCurlError）保留在调用方，CurlResponse 只提供原始 curlCode
- isCurlError 为便捷标记，调用方可直接检查而不需比对 CURLE_OK
- curlErrorStr 由 CurlClient 填充，使调用方（MCPClient、WebSearchTool 等）生成用户可见错误消息时不再调用 `curl_easy_strerror`，彻底消除调用方对 `curl_easy_*` API 的依赖

### 3.3 CurlClient — 静态工具类

```cpp
class CurlClient {
public:
    // 非流式请求（内部 thread_local handle + curl_easy_reset）
    static CurlResponse Post(const CurlRequest& req);
    static CurlResponse Get(const CurlRequest& req);

    // 流式请求。onChunk 接收原始 chunk bytes，SSE 解析逻辑在调用方。
    // 返回 false 中止传输（curl_easy_perform 返回 CURLE_WRITE_ERROR），
    // 是 Model 流式调用实现 mid-stream 取消的钩子。
    static CurlResponse PostStream(const CurlRequest& req,
                                   std::function<bool(const char*, size_t)> onChunk);
    static CurlResponse GetStream(const CurlRequest& req,
                                  std::function<bool(const char*, size_t)> onChunk);

    // URL 编解码（使用内部 thread_local handle）
    static std::string UrlEncode(const std::string& value);
    static std::string UrlDecode(const std::string& value);

    // 全局生命周期
    static void GlobalInit();            // curl_global_init(CURL_GLOBAL_ALL)
    static void GlobalCleanup();         // curl_global_cleanup()
};
```

**设计原则**：
- 静态方法，无实例——所有调用方共享同一 thread_local handle 池
- PostStream/GetStream 为 Phase 5 预留。内部实现与非流式几乎相同，仅在 CURLOPT_WRITEFUNCTION 设为 chunk 分发而非 string append
- UrlEncode/UrlDecode 使用 `curl_easy_escape/curl_easy_unescape`，通过 thread_local handle 调用（不再新建临时 handle）
- GlobalInit 必须在任何 Post/Get 调用之前调用一次（多线程安全）
- GlobalCleanup 必须在所有 Post/Get 调用完成后、所有 thread_local handle 析构后调用

## 4. 内部机制

### 4.1 ThreadCurlHandle — thread_local 持久化 handle wrapper

```cpp
// 内部实现（不暴露到头文件）
class ThreadCurlHandle {
public:
    CURL* Get() {
        if (!handle_) {
            handle_ = curl_easy_init();
        }
        return handle_;
    }
    void Reset() {
        if (handle_) {
            curl_easy_reset(handle_);
        }
    }
    ~ThreadCurlHandle() {
        if (handle_) {
            curl_easy_cleanup(handle_);
        }
    }
private:
    CURL* handle_{nullptr};
};

// 每个 thread 拥有独立的 handle
static thread_local ThreadCurlHandle tlHandle;
```

**关键行为**：
- `Get()` 首次调用时 `curl_easy_init()`，后续返回同一 handle
- `Reset()` 在每次请求前调用，清除上一次请求的选项（URL、headers、timeout 等），但**保留**连接缓存、DNS cache、SSL session cache
- 析构函数在 thread 退出时自动调用 `curl_easy_cleanup()`
- `static thread_local` 确保所有 CurlClient 静态方法共享同一个 per-thread handle

### 4.2 请求执行流程

```
CurlClient::Post(req):
  1. tlHandle.Reset()                              // 清空上一次请求的选项
  2. curl_easy_setopt(CURLOPT_URL, req.url)         // 设置新选项
  3. curl_easy_setopt(CURLOPT_POST, 1L)
  4. curl_easy_setopt(CURLOPT_POSTFIELDS, req.body)
  5. curl_easy_setopt(CURLOPT_POSTFIELDSIZE, req.body.size())
  6. build curl_slist from req.headers
  7. curl_easy_setopt(CURLOPT_HTTPHEADER, slist)
  8. curl_easy_setopt(CURLOPT_WRITEFUNCTION, string-append callback)
  9. curl_easy_setopt(CURLOPT_WRITEDATA, &responseBody)
  10. if (req.connectTimeout)  curl_easy_setopt(CURLOPT_CONNECTTIMEOUT, ...)
  11. if (req.requestTimeout)  curl_easy_setopt(CURLOPT_TIMEOUT, ...)
  12. if (req.followLocation)  curl_easy_setopt(CURLOPT_FOLLOWLOCATION, 1L)
  13. if (!req.sslVerify)      curl_easy_setopt(CURLOPT_SSL_VERIFYPEER, 0L)
  14. if (!req.sslVerify)      curl_easy_setopt(CURLOPT_SSL_VERIFYHOST, 0L)
  15. if (req.userAgent)       curl_easy_setopt(CURLOPT_USERAGENT, ...)
  16. CURLcode res = curl_easy_perform(tlHandle.Get())
  17. curl_easy_getinfo(CURLINFO_RESPONSE_CODE, &statusCode)
  18. curl_slist_free_all(slist)                    // 释放 header list
  19. return CurlResponse{statusCode, responseBody, res, res != CURLE_OK}
```

**流式模式差异**（PostStream）：
- 步骤 8 替换为：`CURLOPT_WRITEFUNCTION` = chunk 分发 callback，`CURLOPT_WRITEDATA` = &chunkCtx
- chunkCtx 内部将原始 bytes 传递给 `onChunk` callback
- response.body 在流式模式下为空（或为 fullText 累积，取决于调用方需求）

### 4.3 curl_global_init / cleanup 时序

```
SessionManager::InitSessionManager()
  → CurlClient::GlobalInit()        // 必须在第一个 curl_easy_init 之前

SessionManager::Shutdown()
  → join 所有线程                   // 确保 thread_local handle 已析构
  → CurlClient::GlobalCleanup()     // 在所有 handle 析构后
```

**atexit 兜底**：在 `GlobalInit()` 中同时注册 `atexit(GlobalCleanup)`，防止 Shutdown 未被调用时 curl_global_cleanup 仍能执行。

**时序约束**：
- `curl_global_init()` 不是线程安全的，必须在任何线程调用 `curl_easy_init()` 之前执行
- `curl_global_cleanup()` 必须在所有 `curl_easy_cleanup()` 之后执行（包括 thread_local 析构）
- thread_local 对象析构顺序：同一 thread 内按声明逆序；不同 thread 的析构时机不确定但都在 thread 退出时

### 4.4 UrlEncode / UrlDecode

```
CurlClient::UrlEncode(value):
  1. CURL* h = tlHandle.Get()       // 使用 thread_local handle
  2. tlHandle.Reset()               // 清空上一次请求的选项（UrlEncode 不需要任何选项）
  3. char* encoded = curl_easy_escape(h, value.c_str(), value.size())
  4. std::string result(encoded)
  5. curl_free(encoded)
  6. return result
```

**注意**：`curl_easy_reset()` 清除所有 CURLOPT 设置但不影响 `curl_easy_escape/curl_easy_unescape` 的行为。UrlEncode/UrlDecode 可以安全地在同一个 handle 上与 Post/Get 交替调用。

## 5. 各子系统迁移要点

### 5.1 HttpMemoryRuntime（Phase 2）

**当前**：DoHttpPostOnce/DoHttpGetOnce 直接操作 curl_easy_*，UrlEncode 新建临时 handle。

**迁移后**：
```cpp
CurlRequest req;
req.url = serverUrl_ + path;
req.headers = BuildHeaders(path);  // Content-Type/Accept/Authorization
req.body = jsonBody;               // POST（空 body = GET）
req.connectTimeout = timeoutSeconds_;
req.requestTimeout = timeoutSeconds_;

CurlResponse resp = CurlClient::Post(req);  // 或 Get(req)
```

- 重试逻辑（HttpPost/HttpGet wrapper + exponential backoff）保留在 HttpMemoryRuntime
- Circuit breaker 保留在 HttpMemoryRuntime
- HttpResponse 结构体改为从 CurlResponse 映射
- UrlEncode 调用改为 `CurlClient::UrlEncode()`
- HttpMemoryRuntime 不再包含任何 `curl_easy_*` 调用

### 5.2 MCPClient（Phase 3）

**当前**：SendRequest 直接操作 curl_easy_*，硬编码 timeout (3s/10s)。

**迁移后**：
```cpp
CurlRequest req;
req.url = endpoint_;
req.headers = BuildHeaders();  // Content-Type/Accept + conditional Mcp-Session-Id + custom headers_
req.body = requestBody;
req.connectTimeout = connectTimeoutSeconds_;  // 从 MCPEndpointConfig 透传
req.requestTimeout = requestTimeoutSeconds_;

CurlResponse resp = CurlClient::Post(req);
```

- 动态 headers 构建：基础 headers + conditional `Mcp-Session-Id: {sessionId_}` + `headers_` vector
- timeout 配置链路（完整配置驱动）：`mcp_servers.json` → app 层 `McpServerEntry.{connect,request}TimeoutSeconds`（默认 3/10）→ `McpServerConfig`（`include/types.h`，公开 API）→ `MCPEndpointConfig`（`resource_manager.cpp` 构造时赋值）→ `MCPClient` 构造透传 → `CurlRequest`。用户可在 `mcp_servers.json` 按 server 覆盖默认值
- JSON-RPC 错误解析保留在 MCPClient

**传输模型说明（事实修正）**：迁移后 MCPClient 的 `SendRequest` 是**离散 POST**——每次 JSON-RPC 请求经 CurlClient 的 thread_local 池化 handle 发一个独立 POST，MCP 会话靠 `Mcp-Session-Id` 请求头维系，而非持久 SSE 长连接订阅。`Accept: application/json, text/event-stream` 仅作内容协商头，当前不消费真正的 SSE 事件流。

这与第一轮 review #17 答复中"MCP 连接的长连接 handle 在初始化时创建并持久持有"的描述有出入——迁移后已简化为离散 POST + 会话头 + thread_local handle 复用，功能等价（Streamable HTTP MCP 的标准语义即是每请求一 POST），且连接复用收益由 CurlClient 的 thread_local handle 承担（同线程连续 MCP 调用复用 TCP/TLS/DNS）。若未来需要真正的持久 SSE 订阅（如服务器主动 push），需在 MCPClient 层另行设计长连接 + 事件分发，不在本次统一范围。

### 5.3 WebSearchTool + WebFetchTool（Phase 4）

**当前**：HttpGet 直接操作 curl_easy_*，UrlEncode/UrlDecode 新建临时 handle，DDG 反爬检测。

**迁移后**：
```cpp
CurlRequest req;
req.url = url;
req.headers = {"User-Agent: Chrome UA", "Accept: text/html,...", "Accept-Language: en-US,..."};
req.followLocation = true;
req.sslVerify = false;
req.requestTimeout = timeoutSec;

CurlResponse resp = CurlClient::Get(req);
```

- UrlEncode/UrlDecode 改为 `CurlClient::UrlEncode/UrlDecode`
- DDG 反爬检测保留在 WebSearchTool
- WebFetchTool 的 userAgent 通过 `req.userAgent` 设置

### 5.4 OpenAIModel + AnthropicModel（Phase 5，最复杂）

**当前**：DoInvokeOnce 直接操作 curl_easy_* + SSE 流式解析（WriteCallback / AnthropicWriteCallback）。

**迁移后**：
```cpp
CurlRequest req;
req.url = config_.baseUrl + "/chat/completions";  // 或 /v1/messages
req.headers = BuildHeaders();  // OpenAI: Authorization Bearer; Anthropic: x-api-key + anthropic-version
req.body = formattedInput;
// 不设 timeout（当前模型不设，curl 默认）

StreamContext ctx;  // SSE 解析逻辑保留在 model 类中
ctx.onChunk = onChunk;
ctx.shouldCancel = shouldCancel;  // 来自 Model::Invoke 的新参数

CurlResponse resp = CurlClient::PostStream(req, [&ctx](const char* data, size_t len) -> bool {
    ProcessSSEChunk(ctx, data, len);  // SSE 行解析逻辑在 model 类中
    // 解析完这批 chunk 后检查取消：已收到的 token 先经 ctx.onChunk 推出再中止
    if (ctx.shouldCancel && ctx.shouldCancel()) {
        ctx.cancelled = true;
        return false;  // 中止传输，curl_easy_perform 返回 CURLE_WRITE_ERROR
    }
    return true;
});

if (ctx.cancelled) {
    // 用 ctx.cancelled 区分"主动中止"与"真 write 错误"：非重试，返回 cancelled
    return {finishReason="cancelled", isFinished=true, isRetryable=false, content=ctx.fullText};
}
```

**关键设计考量**：
- SSE 解析逻辑（StreamContext buffer + line splitting + delta extraction + tool_calls 累积）保留在 OpenAI/Anthropic model 类中，从原 curl WriteCallback 抽出为 `ProcessSSEChunk(ctx, data, len)`
- CurlClient 的 PostStream 只负责传递原始 chunk bytes，onChunk 返回 bool（false 中止）
- 重试逻辑保留在 model 层（onChunk=nullptr 在非最终尝试）；retry loop 每次 attempt 开头检查 shouldCancel，DoInvokeOnce 返回 cancelled 则不重试
- **mid-stream 取消**：`Model::Invoke` 新增 `std::function<bool()> shouldCancel = {}` 参数（默认空 = 不可取消）。AgentWorker::CallModelStream 传 `[this,gen]{ return IsCancelled(gen); }`，使 Cancel() 能在模型流式输出途中真正中止 HTTP 传输，而非仅能在 iteration 之间生效。取消响应 `{finishReason="cancelled", isFinished=true, isRetryable=false}`，上层 ReactLoop 已有 `if (resp.finishReason == "cancelled")` 处理
- 接口变更无 ABI 风险：model 无运行时 .so/.dll 插件（dlopen 仅用于 MemoryRuntime），全部源码集成

### 5.5 FeishuChannel（应用层，不纳入统一范围）

FeishuChannel 位于 `examples/jiuwenClaw/adapters/feishu/`，属应用层（传输层不在核心库内）。按架构约定，应用层不引用核心库内部头（`src/utils/curl_client.h`），且应用层已有自己的 utils 副本（`examples/jiuwenClaw/utils/`），传输实现自洽。

CurlClient 是核心库内部传输实现细节，不进公开 API（`include/`），因此 FeishuChannel 的 2 处 `curl_easy_init` 保留原用法，不在本次统一范围内。若未来需要让应用层复用核心 HTTP 能力，应单独设计一个 curl 无关的公开 HttpClient 接口（`include/`）+ 内部 CurlClient 实现，而非把 `curl_client.h` 提为公开头。

## 6. 文件布局

新增文件：
- `src/utils/curl_client.h` — CurlClient/CurlRequest/CurlResponse 接口
- `src/utils/curl_client.cpp` — 实现（ThreadCurlHandle + thread_local + GlobalInit/Cleanup + Post/Get/PostStream/GetStream/UrlEncode/UrlDecode）
- `CMakeLists.txt` 新增 curl_client.cpp 到 agent_framework 源文件列表

修改文件（Phase 2-5 逐步，仅核心库）：
- Phase 2: `src/memory/http_memory_runtime.cpp`, `src/memory/http_memory_runtime.h`, `src/session/session_manager.cpp`（GlobalInit/Cleanup 调用）
- Phase 3: `src/mcp/mcp_client.cpp`, `src/mcp/mcp_client.h`, `src/mcp/mcp_connection.cpp`, `src/mcp/mcp_connection.h`
- Phase 4: `src/tools/builtin_tools/web_search_tool.cpp`, `src/tools/builtin_tools/web_fetch_tool.cpp`
- Phase 5: `src/models/openai_model.cpp`, `src/models/anthropic_model.cpp`, `include/model.h`, `src/core/agent_worker.cpp`

## 7. 测试策略

每个 Phase 迁移后需验证：
- 迁移后的子系统功能等价（HTTP 请求/响应行为不变）
- thread_local handle 的连接复用行为（同线程连续请求不发起新 TCP 连接）
- GlobalInit/Cleanup 时序正确（无并发 curl_easy_init 风险）
- UrlEncode/UrlDecode 结果与 `curl_easy_escape/curl_easy_unescape` 完全一致

Phase 5（模型迁移）额外需验证：
- SSE 流式解析行为不变（chunk callback 传递原始 bytes，解析逻辑不变）
- 重试期间的 onChunk=nullptr 行为正确
- 流式中断恢复场景

## 8. 风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| thread_local handle 在错误请求后状态异常 | 请求失败 | `curl_easy_reset()` 清除所有状态，handle 可继续使用 |
| thread_local 析构时序与 GlobalCleanup 冲突 | 进程退出时崩溃 | Shutdown 先 join 线程再 GlobalCleanup；atexit 兜底 |
| curl_global_init 未在首线程调用 | 多线程并发初始化 UB | SessionManager::InitSessionManager 在启动时单线程调用 |
| PostStream chunk callback 丢数据 | 模型 SSE 解析不完整 | Phase 5 需严格 SSE 回归测试 |
| UrlEncode/UrlDecode 在 reset 后行为变化 | URL 编码不一致 | curl_easy_reset 不影响 curl_easy_escape 行为 |

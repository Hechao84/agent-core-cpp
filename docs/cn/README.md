# jiuwen-lite 中文文档

`jiuwen-lite` 是一个轻量级、模块化的 C++ AI Agent 框架，用于构建具备工具调用能力的推理型智能体。核心以共享库形式提供，包含模型适配、ReAct 执行、原生 function calling、MCP 集成、技能系统、按会话隔离的上下文、记忆整合以及多会话运行时。HTTP、Web UI、IM 机器人等传输层不放入核心库，而由 `examples/` 下的参考应用实现。

## 构建产物

构建完成后会生成以下产物：

| 产物 | Linux 路径 | Windows 路径 | 描述 |
|------|------------|--------------|------|
| **核心库** | `dist/linux/libagent_framework.so` | `dist/windows/agent_framework.dll` | 框架核心共享库 |
| **头文件** | `include/` | `include/` | 集成库所需的公共 API 头文件 |
| **演示应用** | `dist/linux/jiuwenClaw` | `dist/windows/jiuwenClaw.exe` | 参考应用 |

## 功能特性

### Agent 工作模式

- **ReAct** —— 当前版本的生产执行模式，支持迭代推理、原生工具调用、提示词解析工具调用回退、Observation 与最终回答。
- **Plan-and-Execute / Workflow** —— 暂未实现，后续考虑支持。

### LLM 支持

- OpenAI 兼容 API 格式
- Anthropic API 格式
- 支持 OpenAI 风格 `tools/tool_calls` 与 Anthropic `tool_use/tool_result` 的原生 function calling
- 可通过 `ModelConfig::useNativeFunctionCalling = false` 回退到纯提示词工具调用模式
- 通过 `ModelConfig::extraParams` 传递扩展模型参数，支持 `max_tokens`、`temperature`、`top_p`、`presence_penalty`、`frequency_penalty`、`seed` 等常见 OpenAI 请求字段
- 支持基于 provider 的自定义模型扩展：`ResourceManager::RegisterModel(provider, factory)` + `ModelConfig::provider`

### 内置工具

框架包含 12 个无状态内置工具和 6 个会话级工具。

| 工具 | 作用域 | 用途 |
|------|--------|------|
| `time_info` | 无状态 | 获取当前时间/日期 |
| `web_search` | 无状态 | 网络搜索，并在支持的搜索引擎间回退 |
| `web_fetcher` | 无状态 | 获取网页内容 |
| `read_file` | 无状态 | 读取文件内容 |
| `write_file` | 无状态 | 写入文件 |
| `edit_file` | 无状态 | 原地编辑文件 |
| `list_dir` | 无状态 | 列出目录内容 |
| `glob` | 无状态 | 文件路径模式匹配 |
| `grep` | 无状态 | 文件内容搜索 |
| `exec` | 无状态 | 执行 Shell 命令 |
| `skill_search` | 无状态 | 搜索并加载技能指令 |
| `file_state` | 无状态 | 跟踪文件状态变化 |
| `todo_create` | 会话级 | 创建当前会话的任务列表 |
| `todo_complete` | 会话级 | 标记任务完成并记录结果 |
| `todo_insert` | 会话级 | 插入任务项 |
| `todo_remove` | 会话级 | 移除任务项 |
| `todo_list` | 会话级 | 列出当前任务项 |
| `ask_user` | 会话级 | 在运行过程中向应用/用户请求澄清 |

会话级工具通过 `ResourceManager::RegisterSessionTool` 注册，并通过 `ToolBuildContext` 注入每个会话的资源。

### MCP 集成

- 支持 STDIO、SSE、Streamable HTTP 传输的 Model Context Protocol
- 支持运行时注册、注销、重连 MCP server，并查询已连接 server
- MCP 工具与静态工具分开记录，便于 server 变化时同步工具列表

### 上下文引擎

- **仅内存** —— 临时存储
- **JSON 文件** —— 将消息持久化为 `.json` 文件（默认）
- **数据库** —— 基于 SQLite 的持久化存储
- 存储后端共享 `ContextStorageBase` 通用逻辑
- 自动 token 估算与上下文窗口管理
- 原生持久化 assistant 工具调用和 tool response 元数据

### 会话管理器

- **单一共享 Agent** —— `SessionManager` 拥有一个 live `Agent`，并为每个会话路由独立的 `ContextEngine`
- **每会话串行锁** —— 同一会话的调用串行执行以保证线程安全
- **全局并发门控** —— `AgentConfig::maxConcurrentSessions` 限制并发会话调用数量
- **通道路由** —— `ChannelMessage` 与 `MakeSessionKey(channel, chatId)` 可从 Web、飞书、Telegram、CLI 等传输层生成稳定会话键
- **热重载** —— `ReloadAgent` 可基于更新后的 `AgentConfig` 重建 live agent
- **保留会话 ID**：
  - `__DEFAULT__` —— 未显式传入 `sessionId` 时使用的默认会话
  - `__HEARTBEAT__` —— 周期性后台任务专用
  - `__CRON__` —— 定时任务专用

### Dream 记忆整合

- 空闲会话触发后台记忆整合
- 使用游标记录交互历史
- Dream 处理器分析近期历史、提取关键事实并更新长期记忆
- 在 `MemoryConfig::idleConsolidationSeconds` 配置的空闲时间后自动执行

### 技能系统

- 从包含 `SKILL.md` 与 YAML frontmatter 的目录加载技能
- 渐进式披露：发现阶段只读取元数据，需要时再加载完整指令
- 可通过 `Agent` API 和 jiuwenClaw HTTP API 列出与查看技能

### 配置系统

- 支持 `AgentConfig` 的 JSON 序列化/反序列化
- 持久化覆盖配置默认存放在 `./data/agents.json`
- 应用层可启用轮询 watcher 实现 live reload
- schema 按多 Agent 设计，当前通过 `SessionManager` 同时运行一个 live agent

### 传输层

核心库与传输层解耦。HTTP REST + SSE + Web UI、飞书 WebSocket 机器人适配器由 `examples/jiuwenClaw` 以独立静态库形式提供。下游应用可以复用这些适配器，也可以基于 `SessionManager` 自行实现传输层。

## 系统要求

### 系统依赖

- C++17 编译器（Linux：GCC/Clang，Windows：MSVC）
- CMake >= 3.15
- 带 SSL 支持的 libcurl
- pthread（仅 Linux）
- OpenSSL 可选但推荐，用于 jiuwenClaw 飞书适配器的 `wss://` WebSocket 支持

### 第三方依赖

- nlohmann/json v3.11.3
- SQLite3 3.45.3
- cpp-httplib（jiuwenClaw HTTP/WebSocket 适配器使用）

## 环境配置

### Linux

Ubuntu / Debian：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libcurl4-openssl-dev libssl-dev pkg-config
```

CentOS / RHEL：

```bash
sudo yum groupinstall "Development Tools"
sudo yum install -y cmake libcurl-devel openssl-devel pkg-config
```

### Windows

1. 安装 Visual Studio 2019/2022/2026，并选择“使用 C++ 的桌面开发”工作负载。
2. 安装 vcpkg，并设置 `VCPKG_ROOT` 指向 vcpkg 安装目录。
3. 在 “x64 Native Tools Command Prompt for VS” 中运行 `build_windows.bat`。

## 构建

### Linux

```bash
./build_linux.sh
```

### Windows

```cmd
build_windows.bat
```

脚本会构建第三方依赖、以 Release 模式配置 CMake、构建共享库和 `jiuwenClaw`，并将产物打包到 `dist/<platform>/`。

## 在你的项目中使用该库

### CMake 集成

```cmake
target_link_libraries(your_app PRIVATE agent_framework)
target_include_directories(your_app PRIVATE /path/to/jiuwen-lite/include)
```

### 最小示例

```cpp
#include "include/resource_manager.h"
#include "include/session_manager.h"

using namespace jiuwen;

auto& rm = ResourceManager::GetInstance();

AgentConfig config;
config.id = "my-agent";
config.name = "My Agent";
config.mode = AgentWorkMode::REACT;
config.maxIterations = 10;
config.dataBasePath = "./data";
config.maxConcurrentSessions = 3;
config.defaultTools = rm.GetAvailableTools();

config.modelConfig.baseUrl = "http://your-llm-endpoint/v1";
config.modelConfig.apiKey = "<your-api-key>";
config.modelConfig.modelName = "<your-model-name>";
config.modelConfig.formatType = ModelFormatType::OPENAI;
config.modelConfig.useNativeFunctionCalling = true;
config.modelConfig.extraParams.Set("max_tokens", 4096);
config.modelConfig.extraParams.Set("temperature", 0.2f);

config.contextConfig.sessionId = kDefaultSessionId;
config.contextConfig.storageType = ContextConfig::StorageType::JSON_FILE;
config.memoryConfig.idleConsolidationSeconds = 60;

InitSessionManager(config);

auto result = GetSessionManager().Invoke(
    "user-session-001",
    "你好，你能做什么？",
    [](const std::string& chunk) {
        std::cout << chunk << std::flush;
    }
);

if (!result.success) {
    std::cerr << "错误: " << result.errorMessage << std::endl;
}
```

如需完整应用示例（CLI、HTTP server、Web UI、飞书机器人、通道管理、MCP 管理、心跳与定时任务），请查看 [`examples/jiuwenClaw/`](../../examples/jiuwenClaw/docs/cn/README.md)。

## 测试

```bash
# 构建测试
cmake --build build-linux --target unittest

# 运行测试
./build-linux/unittest
```

`tests/` 目录下还包含额外的 smoke tests，可按需手动编译运行。

## 项目结构

```text
jiuwen-lite/
├── include/                  # 公共 API 头文件
│   ├── agent.h               # Agent 类
│   ├── model.h               # Model 基类、ToolCall、ToolSchema
│   ├── resource_manager.h    # 工具/模型/MCP 注册表
│   ├── session_manager.h     # SessionManager 单例
│   ├── tool.h                # Tool 基类
│   ├── types.h               # 配置结构与运行时类型
│   └── config/               # AgentConfig JSON/store/watcher API
├── src/                      # 核心库实现
│   ├── core/                 # Agent、worker env、Dream、history、会话工具
│   ├── session/              # SessionManager 实现
│   ├── resource_manager/     # ResourceManager 实现
│   ├── workers/              # ReAct worker 与 worker factory
│   ├── models/               # OpenAI、Anthropic 模型实现
│   ├── mcp/                  # MCP client、connection、config manager、MCP tool wrapper
│   ├── tools/                # Tool 基类和内置工具
│   ├── context_engine/       # 内存 / JSON / SQLite 上下文存储
│   ├── skills/               # 技能加载与管理
│   └── utils/                # 日志、编码、提示词工具、工具解析
├── examples/
│   └── jiuwenClaw/           # 参考应用
├── docs/                     # 文档
│   ├── en/
│   └── cn/
├── release_notes/            # 发布说明
├── unittest/                 # CMake 单元测试
├── tests/                    # 独立 smoke tests
└── third_party/              # 第三方源码与头文件
```

## 文档

- 本文件
- [`examples/jiuwenClaw/docs/cn/README.md`](../../examples/jiuwenClaw/docs/cn/README.md)

## 许可证

Apache 2.0

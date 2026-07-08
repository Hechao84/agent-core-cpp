# jiuwenClaw

`jiuwenClaw` 是基于 **jiuwen-lite** 框架构建的参考应用。它展示了如何将 `SessionManager`、`Agent`、`ResourceManager`、工具、MCP server、提示词模板、可热重载配置以及传输适配器组合成一个完整的智能体应用。

> 关于核心框架的整体介绍，请参阅[顶层 README](../../../../docs/cn/README.md)。

## 特性

- **交互式 CLI** —— 默认运行模式，支持多会话命令
- **HTTP REST + SSE 服务** —— 提供 `/api/chat`、`/api/chat/stream`、会话/历史 API 和管理 API
- **内置 Web UI** —— 支持聊天、多会话流式输出、通道、技能、Agent 与 MCP server 管理
- **飞书机器人集成** —— 原生飞书/Lark WebSocket 通道，可通过 `channels.json` 或 Web UI 管理
- **可热重载 Agent 配置** —— 支持 `./data/agents.json`、REST API、CLI reload 命令和可选 watcher
- **MCP server 管理** —— 持久化 `./data/mcp_servers.json`，支持运行时连接/断开/重载与工具同步
- **Dream 记忆整合** —— 空闲会话将历史提炼为长期记忆
- **心跳任务** —— 通过保留会话 `__HEARTBEAT__` 执行周期后台任务
- **定时任务** —— 通过保留会话 `__CRON__` 执行独立调度任务
- **技能发现** —— 通过 API/Web UI 列出和查看基于 `SKILL.md` 的技能
- **应用层工具** —— 定时提醒、桌面通知、Jupyter notebook 编辑

## 演示专属工具

除框架工具外，`jiuwenClaw` 额外注册 3 个应用层工具：

| 工具 | 用途 |
|------|------|
| `cron` | 添加、列出和移除定时提醒 |
| `notify` | 跨平台桌面通知 |
| `notebook_edit` | 编辑 Jupyter 笔记本 |

当 live agent 的工具列表包含会话级工具时，框架还会提供 `todo_*` 和 `ask_user` 工具。

## 传输适配器

`jiuwenClaw` 以独立静态库形式提供两个参考适配器：

| 适配器 | CMake target | 作用 |
|--------|--------------|------|
| HTTP 服务器 | `jiuwenClaw_http_server_adapter` | HTTP REST API、SSE 流式、Web UI、通道 API、Agent API、MCP API |
| 飞书机器人 | `jiuwenClaw_feishu_adapter` | 通过 WebSocket 接入飞书/Lark 机器人 |

核心库不依赖这些适配器。你可以复用、替换它们，或基于 `SessionManager` 实现自己的适配器。

## 目录结构

```text
examples/jiuwenClaw/
├── main.cpp                          # 应用入口、CLI、启动流程、reload 辅助函数
├── reserved_sessions.h               # 应用层保留会话常量（__HEARTBEAT__ / __CRON__）
├── heartbeat_manager.{h,cpp}         # 使用 __HEARTBEAT__ 的心跳模块
├── cron_watcher.{h,cpp}              # 使用 __CRON__ 的定时调度器
├── adapters/
│   ├── http_server/                  # HTTP REST + SSE + Web UI 适配器
│   └── feishu/                       # 飞书 WebSocket 通道适配器
├── channels/                         # 持久化通道注册表和运行时服务
├── mcp/                              # 持久化 MCP server 注册表
├── tools/                            # 演示专属工具
├── utils/                            # 编码、日志、数据目录、字符串辅助函数
├── templates/                        # 提示词模板
├── web/                              # 内置 Web UI
└── docs/
    ├── en/
    └── cn/
```

## 构建与运行

### 构建

```bash
# Linux
./build_linux.sh

# Windows，在 x64 Native Tools Command Prompt for VS 中运行
build_windows.bat
```

构建产物输出到 `dist/<platform>/`。环境依赖与 vcpkg 说明请参阅[顶层 README](../../../../docs/cn/README.md)。

### 运行 CLI

```bash
# Linux
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw

# Windows PowerShell
$env:PATH = "$PWD\dist\windows;$env:PATH"
.\dist\windows\jiuwenClaw.exe
```

### 服务器模式

```bash
# CLI + HTTP server + 已配置通道
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw --server

# 自定义 host/port
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw --server --host 0.0.0.0 --port 9000

# 守护进程模式，不启动 CLI
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw --server --no-cli

# 启用基于轮询的配置热重载
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw --server --watch-config
```

Web UI：

```text
http://localhost:8080
```

## 命令行选项

```text
jiuwenClaw [OPTIONS]
  --server       启用所有集成：HTTP REST API + 已配置通道
  --port <N>     设置服务器端口（默认: 8080）
  --host <IP>    设置服务器地址（默认: 127.0.0.1）
  --no-cli       关闭交互式 CLI，以守护进程运行
  --watch-config 文件变化时自动重载 agent/channel 配置
  --help         显示帮助
```

启用 `--server` 时，会加载并启动 `./data/channels.json` 中启用的通道。

## CLI 命令

| 命令 | 说明 |
|------|------|
| `/exit` | 退出程序 |
| `/session <id>` | 切换到指定会话，不存在则创建 |
| `/sessions` | 列出活跃会话 |
| `/reload` | 重载 `agents.json` 并协调 `channels.json` |
| `/reload agent [<id>]` | 重载指定 Agent 配置，默认 live agent |
| `/reload channels` | 使用 `channels.json` 协调运行中的通道 |
| `/config show [<id>]` | 打印生效后的 Agent 配置 |

## HTTP API

服务器提供聊天与运行时管理 JSON API：

| API | 作用 |
|-----|------|
| `GET /api/health` | 健康检查与会话数量 |
| `GET /api/sessions` | 列出非保留会话 |
| `POST /api/sessions` | 创建会话 |
| `DELETE /api/sessions/{id}` | 删除非保留会话 |
| `POST /api/sessions/{id}/cancel` | 请求取消 |
| `GET /api/sessions/{id}/history` | 读取会话消息历史 |
| `POST /api/chat` | 非流式聊天 |
| `POST /api/chat/stream` | SSE 流式聊天 |
| `GET /api/tools` | 列出可用工具 |
| `GET /api/skills` / `GET /api/skills/{id}` | 列出和查看技能 |
| `GET/PUT/DELETE /api/agents/{id}` | 管理 Agent 覆盖配置 |
| `POST /api/agents/reload` | 从磁盘重载 Agent 配置 |
| `GET/POST/PUT/DELETE /api/channels` | 管理传输通道 |
| `POST /api/channels/reload` | 从磁盘重载通道配置 |
| `GET/POST/PUT/DELETE /api/mcp_servers` | 管理 MCP servers |
| `POST /api/mcp_servers/reload` | 从磁盘重载 MCP server 配置 |
| `POST /api/answer` | 响应待处理的 `ask_user` 请求 |

`/api/chat/stream` 会输出带类型的 SSE 事件，包括流式 token、状态、工具调用、工具响应、`ask_user` 问题和最终完成事件。

## 配置文件

运行时状态默认保存在 `./data` 下。

| 文件 | 作用 |
|------|------|
| `agents.json` | Agent 覆盖配置。缺失字段回退到 `BuildAgentConfig()` 中的代码默认值 |
| `channels.json` | 飞书与未来通道定义 |
| `mcp_servers.json` | 持久化 MCP server 定义 |

### Agent 配置

`agents.json` 已按多 Agent 设计。通过 `SessionManager` 运行一个 live agent，但配置存储会保留所有 Agent 条目。

示例覆盖配置：

```json
{
  "version": 1,
  "agents": [
    {
      "id": "demo-agent",
      "modelConfig": {
        "baseUrl": "<your-llm-endpoint>",
        "apiKey": "<your-api-key>",
        "modelName": "<your-model-name>",
        "formatType": "openai",
        "useNativeFunctionCalling": true,
        "extraParams": {
          "max_tokens": 4096,
          "temperature": 0.2
        }
      },
      "maxIterations": 100,
      "mcpServerIds": ["amap"]
    }
  ]
}
```

### 通道配置

通道定义持久化在 `./data/channels.json`，可通过 Web UI 或 REST API 编辑。

```json
[
  {
    "id": "my-feishu-bot",
    "name": "My Feishu Bot",
    "type": "feishu",
    "enabled": true,
    "params": {
      "appId": "<your-feishu-app-id>",
      "appSecret": "<your-feishu-app-secret>"
    }
  }
]
```

### MCP Server 配置

MCP server 持久化在 `./data/mcp_servers.json`。支持 `streamable-http-client`、`sse`、`stdio` 三种传输。

```json
[
  {
    "id": "amap",
    "name": "Amap MCP",
    "enabled": true,
    "type": "streamable-http-client",
    "url": "https://mcp.amap.com",
    "endpoint": "/mcp?key=<your-amap-key>",
    "headers": {}
  }
]
```

编辑 MCP server 后，可通过 Web UI、REST API 重载，或重启应用。server 变化后 live agent 会同步 MCP 工具。

## 心跳任务

心跳管理器周期性检查 `./data/HEARTBEAT.md`。声明在 `## Active Tasks` 下的任务会被评估，并在到期时通过 `__HEARTBEAT__` 会话分发。

```markdown
## Active Tasks

- 每天检查一次项目更新并生成摘要
- 每小时监控磁盘使用情况
```

如果该小节为空或只包含注释，本次心跳周期会跳过。

## 定时任务

`CronWatcher` 是独立调度器，触发时通过 `__CRON__` 会话调用 Agent。

| 类型 | 说明 | 参数 |
|------|------|------|
| `one-time` | 单次执行后移除 | `at` ISO 时间戳 |
| `recurring` | 固定间隔重复触发 | `every_seconds` |
| `cron` | 按 cron 表达式触发 | `cron_expr`，如 `"0 9 * * 1-5"` |

用户通常通过自然语言创建定时任务，例如：

```text
10 分钟后提醒我喝水。
每周一到周五早上 9 点提醒晨会。
```

## 提示词模板

`jiuwenClaw` 使用模块化提示词模板，并由 `REACT_SYSTEM.md` 引用。

| 文件 | 职责 | 占位符 |
|------|------|--------|
| `AGENTS.md` | Agent 能力清单和行为规则 | `{$agents}` |
| `SOUL.md` | Agent 人格与沟通风格 | `{$soul}` |
| `USER.md` | 用户画像与偏好 | `{$user}` |
| `TOOLS.md` | 工具使用策略 | `{$tools_md}` |
| `MEMORY.md` | Dream 记忆整合提示词 | — |
| `REACT_SYSTEM.md` | 主模板，包含运行时上下文 | 入口 |

框架还会注入长期记忆，并在启用时注入当前会话 todo list。

## OpenAI 兼容模型 Provider

OpenAI 兼容后端可直接使用内置 `OpenAIModel`。`max_tokens`、`temperature`、`top_p` 等常见请求字段可通过 `ModelConfig::extraParams` 提供，不需要自定义 provider。

## 基于 jiuwen-lite 构建自己的 Agent

1. 在 `examples/` 下或你自己的仓库中创建新应用目录。
2. 定义提示词模板并配置到 `AgentConfig::promptTemplates`。
3. 注册应用工具、可选会话级工具和可选模型 provider。
4. 按需加载 MCP servers。
5. 使用 `InitSessionManager(config)` 初始化运行时。
6. 通过 `GetSessionManager().Invoke(sessionId, query, callback)` 驱动交互。
7. 将传输适配器作为应用层代码接入。

## 文档

- 本文件
- [框架 README](../../../../docs/cn/README.md)

## 快速链接

- [框架 README](../../../../docs/cn/README.md)
- [发布说明](../../../../release_notes/)

## 许可证

Apache 2.0

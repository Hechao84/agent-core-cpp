# jiuwenClaw

`jiuwenClaw` 是基于 **jiuwen-lite** 框架构建的参考演示应用。它展示了如何将核心库
（`SessionManager`、`Agent`、`ResourceManager`）与具体的传输适配器（HTTP REST + SSE + Web UI、
飞书 WebSocket 机器人）、应用层能力（心跳、定时任务）、提示词模板和自定义模型 provider 串联起来。

> 关于核心库的整体介绍，请参阅[顶层 README](../../../README.md)。

## 特性

- **多会话 CLI** —— 通过 `/session <id>` / `/sessions` 随时切换上下文
- **ReAct 推理** —— 工具增强的思维链
- **Dream 记忆整合** —— 空闲会话自动将历史提炼为长期记忆
- **心跳任务** —— 使用保留会话 `__HEARTBEAT__` 执行的周期任务
- **定时任务 (`CronWatcher`)** —— 使用保留会话 `__CRON__` 的独立调度器
- **技能系统** —— 按需发现并加载 `SKILL.md`
- **自定义模型 provider** —— 内置 `ArkCodeModel` 作为厂商定制（OpenAI 兼容）扩展示例
- **双运行模式** —— 交互式 CLI 和/或服务器模式（`--server`，自带 Web UI）
- **通道持久化** —— 通道定义保存在 `./data/channels.json`，可通过 Web UI 编辑

### 演示专属工具

除核心库提供的 12 个内置工具外，`jiuwenClaw` 额外注册了 3 个应用层工具：

| 工具 | 用途 |
|------|------|
| `cron` | 添加、列出和移除定时提醒 |
| `notify` | 跨平台桌面通知 |
| `notebook_edit` | 编辑 Jupyter 笔记本 |

### 传输适配器

`jiuwenClaw` 以独立静态库形式提供两个参考适配器：

| 适配器 | CMake target | 作用 |
|--------|--------------|------|
| HTTP 服务器 | `jiuwenClaw_http_server_adapter` | HTTP REST API + SSE 流式 + 内置 Web UI |
| 飞书机器人 | `jiuwenClaw_feishu_adapter` | 通过 WebSocket 接入飞书（Lark）机器人 |

核心库不依赖任何适配器 —— 你可以直接复用、替换它们，或基于 `SessionManager` 编写自己的适配器。

## 目录结构

```
examples/jiuwenClaw/
├── main.cpp                          # 应用入口（SessionManager 驱动）
├── heartbeat_manager.{h,cpp}         # 心跳模块（使用 __HEARTBEAT__ 会话）
├── cron_watcher.{h,cpp}              # 定时任务调度（使用 __CRON__ 会话）
├── adapters/
│   ├── http_server/                  # HTTP REST + SSE 适配器
│   │   ├── http_server.{h,cpp}
│   └── feishu/                       # 飞书 WebSocket 机器人适配器
│       ├── feishu_bot.{h,cpp}
│       └── feishu_channel.{h,cpp}
├── channels/
│   └── channel_manager.{h,cpp}       # 通道定义持久化到 channels.json
├── models/
│   └── ark_code_model.{h,cpp}        # 自定义 OpenAI 兼容 provider 示例
├── tools/
│   ├── cron_tool.{h,cpp}             # 定时任务管理工具
│   ├── notify_tool.{h,cpp}           # 桌面通知工具
│   └── notebook_edit_tool.{h,cpp}    # Jupyter notebook 编辑工具
├── utils/                            # 本地辅助函数（编码、日志、字符串等）
├── templates/                        # 提示词模板（见下文）
├── web/
│   └── index.html                    # 内置 Web UI（--server 启用时提供）
└── doc/
    ├── en/                           # 英文文档
    └── cn/                           # 中文文档
```

## 构建与运行

### 构建

```bash
# Linux
./build_linux.sh

# Windows（在 "x64 Native Tools 命令提示" 中运行）
build_windows.bat
```

构建产物输出到 `dist/<platform>/`。环境依赖与 vcpkg 说明请参阅[顶层 README](../../../README.md)。

### 运行

```bash
# Linux（默认：交互式 CLI）
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw

# Windows
$env:PATH = "$PWD\dist\windows;$env:PATH"
.\dist\windows\jiuwenClaw.exe
```

### 服务器模式（HTTP API + Web UI + 已配置的通道）

```bash
# Linux
./dist/linux/jiuwenClaw --server

# 自定义主机与端口
./dist/linux/jiuwenClaw --server --host 0.0.0.0 --port 9000

# 守护进程模式（关闭 CLI，仅 HTTP + 通道）
./dist/linux/jiuwenClaw --server --no-cli
```

Web UI 访问地址：
```
http://localhost:8080
```

### 命令行选项

```
jiuwenClaw [OPTIONS]
  --server       启用 HTTP REST API + Web UI + 所有已配置的通道
  --port <N>     设置服务器端口（默认: 8080）
  --host <IP>    设置服务器主机地址（默认: 127.0.0.1）
  --no-cli       关闭交互式 CLI（仅以守护进程运行，需配合 --server）
  --help         显示本帮助信息
```

启用 `--server` 时，`./data/channels.json` 中所有 `enabled=true` 的通道会被加载并启动。

## CLI 命令

CLI 启动后可使用以下斜杠命令：

| 命令 | 说明 |
|------|------|
| `/exit` | 退出程序 |
| `/session <id>` | 切换到指定会话（不存在则自动创建） |
| `/sessions` | 列出活跃会话并标记当前会话 |

## 通道配置

通道定义保存在 `./data/channels.json`，有两种修改方式：

- **通过 Web UI**：访问 `/api/channels`（需 `--server` 模式）
- **手动编辑** JSON 文件

每条配置包含 `type`（当前支持 `feishu`）、`id`、人类可读的 `name`、`enabled` 开关以及
`params` 字典。飞书机器人示例：

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

CLI 与 HTTP 传输**不**出现在此文件中 —— 它们由上方的命令行参数控制。

## 心跳任务

心跳管理器周期性检查 `./data/HEARTBEAT.md`。声明在 `## Active Tasks` 小节下的任务会被评估，
并在到达触发条件时通过 `__HEARTBEAT__` 保留会话分发到 Agent：

```markdown
## Active Tasks

- 每天检查一次项目更新并生成摘要
- 每小时监控磁盘使用情况
```

如果该小节为空或仅包含注释，本次心跳周期将被跳过。

## 定时任务

`CronWatcher` 是一个独立调度器，通过 `__CRON__` 保留会话在触发时调用大模型，**不依赖**心跳模块。

| 类型 | 说明 | 参数 |
|------|------|------|
| `one-time` | 单次执行后自动移除 | `at`（ISO 时间戳） |
| `recurring` | 按固定时间间隔循环触发 | `every_seconds` |
| `cron` | 按 cron 表达式触发 | `cron_expr`（例如 `"0 9 * * 1-5"`） |

大多数定时任务可通过与 Agent 的自然语言交互创建：

```
"10 分钟后提醒我喝水"
"每周一到周五早上 9 点提醒晨会"
```

## 配置（位于 `main.cpp`）

按需修改 `examples/jiuwenClaw/main.cpp` 中的以下片段：

1. **LLM 接入**
   ```cpp
   config.modelConfig.baseUrl = "<your-llm-endpoint>/v1";
   config.modelConfig.apiKey = "<your-api-key>";
   config.modelConfig.modelName = "<your-model-name>";
   config.modelConfig.formatType = ModelFormatType::OPENAI;
   // config.modelConfig.provider = "ark_code"; // 使用自定义 provider 时
   ```

2. **高德 MCP key**（Streamable HTTP MCP 示例）
   ```cpp
   "endpoint": "/mcp?key=<your-amap-key>"
   ```

3. **技能目录**
   ```cpp
   config.skillDirectory = "./my_skills";
   ```

## 提示词模板

`jiuwenClaw` 采用模块化提示词模板系统，每个文件职责清晰，由 `REACT_SYSTEM.md` 通过占位符引用：

| 文件 | 职责 | 占位符 |
|------|------|--------|
| `AGENTS.md` | Agent 能力清单与行为准则 | `{$agents}` |
| `SOUL.md` | Agent 人格与沟通风格 | `{$soul}` |
| `USER.md` | 用户画像与偏好 | `{$user}` |
| `TOOLS.md` | 工具使用策略（安全规则、搜索策略） | `{$tools_md}` |
| `REACT_SYSTEM.md` | 主模板 —— 组装所有组件 + 运行时上下文 | (入口) |
| `MEMORY.md` | Dream 记忆整合所用模板 | — |

## 自定义模型 Provider

`jiuwenClaw` 通过 `ArkCodeModel` 演示 provider 机制 —— 它是一个 OpenAI 兼容后端，但需要对
`role=tool` 消息做特殊处理：

```cpp
// 1. 用唯一名称注册 provider
auto& rm = ResourceManager::GetInstance();
rm.RegisterModel("ark_code", [](const ModelConfig& cfg) {
    return std::make_unique<ArkCodeModel>(cfg);
});

// 2. 通过 ModelConfig::provider 选用
config.modelConfig.provider = "ark_code";
config.modelConfig.baseUrl = "<your-endpoint>/v3";
config.modelConfig.apiKey = "<your-api-key>";
config.modelConfig.modelName = "<your-model-name>";
```

当 `provider` 非空时，`ResourceManager::CreateModel` 会派发到已注册的工厂；否则回退到由
`ModelConfig::formatType` 选择的内置 OpenAI/Anthropic 实现。

## 基于 jiuwen-lite 构建你自己的 Agent

要创建基于 jiuwen-lite 的新 Agent 应用：

1. 在 `examples/` 下创建新目录（或在你自己的仓库中）
2. 配置 `promptTemplates` 指向你的模板文件
3. 注册需要的工具（内置、MCP 或自定义）
4. （可选）通过 `ResourceManager::RegisterModel` 注册自定义模型 provider
5. 调用 `InitSessionManager(config)` 初始化运行时
6. 通过 `GetSessionManager().Invoke(sessionId, query, callback)` 驱动交互

jiuwen-lite 核心库不包含任何业务逻辑 —— 所有定制（提示词、工具、技能、传输、通道）都属于你的应用。

## 文档

- **English**: [README.md](../../README.md)
- **中文**: 本文件

## 快速链接

- [框架主页](../../../../README.md)
- [发布说明](../../../../release_notes/)

## 许可证

Apache 2.0

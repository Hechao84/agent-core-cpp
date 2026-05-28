# Jiuwen Claw

Jiuwen Claw 是一个基于 **jiuwen-lite** 框架构建的 AI 智能体应用。它演示了如何利用框架的核心能力（上下文管理、记忆整理、工具调用、技能系统）构建一个功能完整的 AI 助手。

## 特性

- **多会话管理** — 独立的会话上下文，支持 CLI 会话切换 (`/session`, `/sessions`)
- **ReAct 推理** — 通过思维链进行工具调用和问题解决
- **Dream 记忆整合** — 空闲时自动分析对话历史，提取关键事实更新长期记忆
- **心跳任务 (Heartbeat)** — 定期检查并执行周期性任务（使用独立会话 `__HEARTBEAT__`）
- **定时任务 (CronWatcher)** — 独立运行的定时任务模块，支持多种触发方式（使用独立会话 `__CRON__`）
- **技能系统** — 搜索和加载专业技能指导 (SKILL.md)
- **自定义模型提供者** — 演示基于 Provider 的自定义模型扩展 (ArkCode model)
- **工具集成**：
  - 文件管理（读写、编辑、搜索）
  - Web 搜索和抓取
  - 高德地图 MCP 服务
  - 定时提醒 (cron)
  - 桌面通知 (notify)
  - 笔记管理
- **双运行模式**：
  - **CLI 模式** — 交互式命令行界面
  - **服务器模式** — 带 SSE 流式输出的 Web UI (`--server`)

## 目录结构

```
jiuwenClaw/
├── main.cpp                    # 应用入口（SessionManager 驱动）
├── heartbeat_manager.h/cpp     # 心跳管理模块（使用 __HEARTBEAT__ 会话）
├── cron_watcher.h/cpp          # 定时任务模块（使用 __CRON__ 会话）
├── models/                     # 自定义模型提供者
│   └── ark_code_model.h/cpp    # ArkCode 模型提供者示例
├── utils/                      # 工具函数
│   ├── encoding.h/cpp          # UTF-8/GBK 编码转换
│   ├── logger.h/cpp            # 日志工具
│   └── string_utils.h/cpp      # 字符串处理工具
├── templates/                  # 提示词模板
│   ├── AGENTS.md               # Agent 行为指令
│   ├── SOUL.md                 # Agent 个性与价值观
│   ├── USER.md                 # 用户画像与偏好
│   ├── TOOLS.md                # 工具使用策略
│   ├── REACT_SYSTEM.md         # 系统提示词组装模板
│   └── MEMORY.md               # Dream 记忆整合模板
├── tools/                      # 定制化工具
│   ├── cron_tool.h/.cpp        # 定时任务管理工具
│   ├── notify_tool.h/.cpp      # 桌面通知工具
│   └── notebook_edit_tool.h/.cpp
└── skills/                     # (用户自定义) 技能文件
```

## 提示词模板

Jiuwen Claw 使用模块化的提示词模板系统，每个文件职责明确：

| 文件 | 职责 | 占位符 |
|------|------|--------|
| `AGENTS.md` | Agent 能力清单、行为准则 | `{$agents}` |
| `SOUL.md` | Agent 个性、沟通风格 | `{$soul}` |
| `USER.md` | 用户画像、技术背景 | `{$user}` |
| `TOOLS.md` | 工具使用策略（安全规则、搜索策略） | `{$tools_md}` |
| `REACT_SYSTEM.md` | 主模板，组装所有组件 + 运行时上下文 | (主入口) |

## 快速开始

### 前置要求

- C++17 编译器
- CMake 3.15+
- Linux (WSL) 或 Windows

### 编译

```bash
# Linux
./build_linux.sh

# Windows (VS 开发人员命令提示)
build_windows.bat
```

### 运行

```bash
# Linux（默认 CLI 模式）
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw

# Linux（服务器模式，带 Web UI）
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw --server
```

### 命令行选项

```
jiuwenClaw [OPTIONS]
  --server     启动带 Web UI 的 Agent 服务器 (默认: 127.0.0.1:8080)
  --port <N>   设置服务器端口 (默认: 8080)
  --host <IP>  设置服务器主机地址 (默认: 127.0.0.1)
  --cli        启动 CLI 模式 (默认)
  --help       显示帮助信息
```

### 配置

在 `main.cpp` 中修改以下配置：

1. **LLM 端点**：
   ```cpp
   config.modelConfig.baseUrl = "your-endpoint/v1";
   config.modelConfig.apiKey = "your-api-key";
   ```

2. **高德地图 MCP Key**：
   ```cpp
   "endpoint": "/mcp?key=your-amap-key"
   ```

3. **技能目录**：
   ```cpp
   config.skillDirectory = "./my_skills";
   ```

## 心跳任务 (Heartbeat)

心跳机制定期检查 `data/HEARTBEAT.md`。在 "Active Tasks" 部分添加任务后，Agent 会自动判断并执行（通过 `__HEARTBEAT__` 会话调用 SessionManager）：

```markdown
## Active Tasks

- 每天检查一次项目更新并生成摘要
- 每小时监控磁盘使用情况
```

如果该部分为空或仅包含注释，Agent 将跳过心跳检查。

## 定时任务 (CronWatcher)

CronWatcher 是一个独立运行的模块，使用 `__CRON__` 会话通过 SessionManager 调用，检查到触发事件后会调用大模型处理，**不依赖心跳模块**。

### 定时任务类型

| 类型 | 说明 | 参数 |
|------|------|------|
| `one-time` | 单次执行，触发后自动从列表中移除 | `at`（ISO 时间） |
| `recurring` | 按固定间隔循环执行 | `every_seconds` |
| `cron` | 按 cron 表达式在特定时刻触发 | `cron_expr`（如 `"0 9 * * 1-5"`） |

### 示例

```bash
# 通过 Agent 的自然语言指令创建定时提醒
# "10分钟后提醒我喝水"
# "每周一到周五早上9点提醒晨会"
```

## CLI 命令

启动后可使用以下命令：

| 命令 | 说明 |
|------|------|
| `/exit` | 退出程序 |
| `/session <id>` | 切换到指定会话（不存在则自动创建） |
| `/sessions` | 列出所有活跃会话及当前会话 |

## 记忆管理

- **Dream 自动整合**：对话空闲 60 秒后，DreamProcessor 自动分析历史并更新 `MEMORY.md`
- **History Store**：记录每次交互（角色、内容、工具调用数），支持游标追踪

## 自定义新智能体

要基于 jiuwen-lite 创建你自己的智能体：

1. 在 `examples/` 下创建新目录
2. 配置 `promptTemplates` 指向你的模板文件
3. 注册你需要的工具 (MCP 或自定义)
4. 调用 `InitSessionManager(config)` 初始化会话管理器
5. 通过 `GetSessionManager().Invoke(sessionId, query, callback)` 发起对话
6. 运行构建脚本即可

jiuwen-lite 框架本身不包含业务逻辑，所有定制内容（提示词、工具、技能）都在各自的智能体目录下管理。

## 许可证

Apache 2.0

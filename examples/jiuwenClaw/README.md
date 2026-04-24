# Jiuwen Claw

Jiuwen Claw 是一个基于 **jiuwen-lite** 框架构建的 AI 智能体应用。它演示了如何利用框架的核心能力（上下文管理、记忆整理、工具调用、技能系统）构建一个功能完整的 AI 助手。

## 特性

- **ReAct 推理**：通过思维链进行工具调用和问题解决
- **记忆管理**：自动睡时记忆整理，长期记忆持久化
- **心跳任务 (Heartbeat)**：定期检查并执行周期性任务
- **定时任务 (CronWatcher)**：独立运行的定时任务模块，支持多种触发方式
- **技能系统**：搜索和加载专业技能指导 (SKILL.md)
- **工具集成**：
  - 文件管理（读写、编辑、搜索）
  - Web 搜索和抓取
  - 地图服务（高德 MCP）
  - 定时提醒 (cron)
  - 桌面通知 (notify)
  - 笔记管理

## 目录结构

```
jiuwenClaw/
├── main.cpp                    # 应用入口
├── heartbeat_manager.h/cpp     # 心跳管理模块
├── cron_watcher.h/cpp          # 定时任务模块（独立运行）
├── templates/                  # 提示词模板
│   ├── AGENTS.md               # Agent 行为指令
│   ├── SOUL.md                 # Agent 个性与价值观
│   ├── USER.md                 # 用户画像与偏好
│   ├── TOOLS.md                # 工具使用策略
│   └── REACT_SYSTEM.md         # 系统提示词组装模板
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
- CMake 3.14+
- Linux (WSL) 或 Windows + MinGW

### 编译

```bash
# Linux
./build_linux.sh
```

### 运行

```bash
# Linux
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw
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

心跳机制定期检查 `data/HEARTBEAT.md`。在 "Active Tasks" 部分添加任务后，Agent 会自动判断并执行：

```markdown
## Active Tasks

- 每天检查一次项目更新并生成摘要
- 每小时监控磁盘使用情况
```

如果该部分为空或仅包含注释，Agent 将跳过心跳检查。

## 定时任务 (CronWatcher)

CronWatcher 是一个独立运行的模块，拥有自己的 Agent 实例，检查到触发事件后会直接调用大模型处理，**不依赖心跳模块**。

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

## 记忆管理

- **自动**：对话空闲 60 秒后，自动整理记忆到 `MEMORY.md`
- **手动**：通过 Agent 的 `UpdateMemory()` / `ClearMemory()` 接口操作

## 自定义新智能体

要基于 jiuwen-lite 创建你自己的智能体：

1. 在 `examples/` 下创建新目录
2. 配置 `promptTemplates` 指向你的模板文件
3. 注册你需要的工具 (MCP 或自定义)
4. 运行构建脚本即可

jiuwen-lite 框架本身不包含业务逻辑，所有定制内容（提示词、工具、技能）都在各自的智能体目录下管理。

# jiuwen-lite 中文文档

一个轻量级、模块化的 C++ AI 代理框架库，用于构建推理代理。提供共享库，支持多种 LLM 提供商、代理工作模式、内置工具、MCP 集成、技能管理和多通道通信。

## 构建产物

构建完成后，将生成以下产物：

| 产物 | Linux 路径 | Windows 路径 | 描述 |
|------|-----------|--------------|------|
| **核心库** | `dist/linux/libagent_framework.so` | `dist/windows/agent_framework.dll` | 框架核心共享库 |
| **头文件** | `include/` | `include/` | 集成库所需的公共 API 头文件 |
| **演示应用** | `dist/linux/jiuwenClaw` | `dist/windows/jiuwenClaw.exe` | 展示如何使用框架的示例应用 |

## 功能特性

### 代理工作模式
- **ReAct** — 迭代推理和行动循环
- **Plan-and-Execute** — 先生成计划，然后按顺序执行每个步骤
- **Workflow** — 基于节点的流程执行，可配置步骤 *(规划中)*

### LLM 支持
- OpenAI 兼容 API 格式
- Anthropic API 格式
- **基于 Provider 的自定义模型扩展** — 可扩展的模型提供商系统，用于添加新的 LLM 后端

### 内置工具 (12个)
| 工具 | 用途 |
|------|------|
| `time_info` | 获取当前时间/日期 |
| `web_search` | 网络搜索 |
| `web_fetcher` | 获取网页内容 |
| `read_file` | 读取文件内容 |
| `write_file` | 写入文件 |
| `edit_file` | 原地编辑文件 |
| `list_dir` | 列出目录内容 |
| `glob` | 文件模式匹配 |
| `grep` | 文件内容搜索 |
| `exec` | 执行 Shell 命令 |
| `skill_search` | 搜索和加载技能指令 |
| `file_state` | 跟踪文件状态变化 |

### jiuwenClaw 演示专属工具 (3个)
| 工具 | 用途 |
|------|------|
| `cron` | 安排、列出和移除提醒 |
| `notify` | 跨平台桌面通知 |
| `notebook_edit` | 编辑 Jupyter 笔记本 |

### MCP 集成
- 支持 STDIO、SSE 和 Streamable HTTP 传输的模型上下文协议
- 可连接外部 MCP 服务器以扩展工具能力

### 上下文引擎
- **仅内存** — 临时存储
- **JSON 文件** — 将消息持久化为 `.json` 文件（默认）
- **数据库** — 基于 SQLite 的持久化存储
- 通用基类 (`ContextStorageBase`) 用于共享逻辑，带有专门的后端
- 自动 token 估算和上下文窗口管理

### 会话管理器
- **多会话支持** — 每个会话隔离对话上下文
- **每会话锁定** — 串行化同一会话调用以确保线程安全
- **并发门控** — 全局限制并发会话调用数 (`maxConcurrentSessions`)
- **通道路由** — 从通道类型（websocket、feishu、telegram、cli）+ chatId 自动派生会话密钥
- **保留会话** — 用于内部任务的内置会话（`__HEARTBEAT__`、`__CRON__`、`__UNIFIED__`）
- **多会话历史跟踪** — 每会话历史条目，带会话 ID 关联

### Dream 记忆整合
- **后台整合** — 空闲会话触发 Dream 处理器整合记忆
- **历史存储** — 记录交互历史，带基于游标的跟踪
- **两阶段处理** — 分析历史、提取关键事实、更新长期记忆
- **自动化** — 无需手动调用 `UpdateMemory()`；在可配置的空闲超时后自动执行

### 技能系统
- 从目录结构加载技能，支持 `SKILL.md` 和 YAML frontmatter
- 渐进式披露：元数据始终可用，完整指令按需加载

### 多通道通信
- **Web API + Web UI** — HTTP REST API，带 SSE 流式传输和内置 Web 界面
  - 实时流式响应
  - UI 中支持多会话
  - 工具调用可视化
- **飞书 (Lark) WebSocket 通道** — 通过 WebSocket 原生集成飞书机器人
  - 支持私信和群聊
  - 消息卡片格式化
- **通道管理器** — Web UI 用于通道配置和管理

## 系统要求

### 系统依赖
- C++17 编译器（Linux 上的 GCC/Clang，Windows 上的 MSVC）
- CMake >= 3.15
- libcurl（带 SSL 支持）
- pthread（仅 Linux）

### 第三方依赖（由脚本自动构建）
- nlohmann/json v3.11.3（仅头文件）
- SQLite3 3.45.3（用于 DB 上下文存储）

## 环境配置

在构建之前，请确保您的开发环境满足以下要求。

### Linux
**Ubuntu / Debian:**
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libcurl4-openssl-dev pkg-config
```
**CentOS / RHEL:**
```bash
sudo yum groupinstall "Development Tools"
sudo yum install -y cmake libcurl-devel pkg-config
```

### Windows
Windows 构建脚本旨在为您处理大部分复杂性，前提是您的环境已正确设置。

1.  **前提条件**
    - **Visual Studio 2019/2022/2026（社区版或更高版本）**。
    - **使用 C++ 的桌面开发工作负载**（必须包含 MSVC 工具和 Windows SDK）。

2.  **环境变量 & vcpkg**
    - 项目使用 **vcpkg** 清单模式自动管理依赖项（如 `libcurl`）。确保根目录的 `vcpkg.json` 已在 git 中跟踪。
    - 确保环境变量 **`VCPKG_ROOT`** 设置为您的 vcpkg 安装目录（例如 `C:\vcpkg` 或 `D:\tools\vcpkg`）。
    - *注意：如果您还没有安装 vcpkg，请克隆它并运行 `bootstrap-vcpkg.bat`。*

3.  **构建**
    - **重要提示：** 始终从 **"VS 的开发人员命令提示"**（或 VS20xx **"x64 Native Tools 命令提示"**）运行构建脚本，因为脚本需要 `cl.exe` 和标准 MSVC 环境变量。

## 构建

### Linux

```bash
./build_linux.sh
```

### Windows
```cmd
build_windows.bat
```
*（注意：运行脚本前请确保 `VCPKG_ROOT` 已定义。）*

两个脚本都会：
1. 构建第三方依赖项（nlohmann/json、SQLite3）
2. 配置 CMake 为 Release 模式
3. 构建共享库和演示应用程序
4. 将输出打包到 `dist/<platform>/`

## 运行演示

### Linux
```bash
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw
```

### Windows
```powershell
$env:PATH = "$PWD\dist\windows;$env:PATH"
.\dist\windows\jiuwenClaw.exe
```

### Web UI 模式
启用 Web 服务器模式启动：
```bash
# Linux
./dist/linux/jiuwenClaw --server

# 自定义主机/端口
./dist/linux/jiuwenClaw --server --host 0.0.0.0 --port 9000
```

访问以下地址使用 UI：
```
http://localhost:8080
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

## 在您的项目中使用该库

### CMake 集成

```cmake
# 链接到框架库
target_link_libraries(your_app PRIVATE agent_framework)
target_include_directories(your_app PRIVATE /path/to/jiuwen-lite/include)
```

### 示例：使用 SessionManager 创建代理

请参阅 `examples/jiuwenClaw/main.cpp` 获取完整的工作示例。核心使用模式为：

```cpp
#include "include/agent.h"
#include "include/resource_manager.h"
#include "include/session_manager.h"

using namespace jiuwen;

// 1. 获取资源管理器单例
auto& rm = ResourceManager::GetInstance();

// 2. 配置代理
AgentConfig config;
config.id = "my-agent";
config.name = "My Agent";
config.mode = AgentWorkMode::REACT;
config.maxIterations = 5;

// 3. 配置模型
config.modelConfig.baseUrl = "http://your-llm-endpoint/v1";
config.modelConfig.apiKey = "your-api-key";
config.modelConfig.modelName = "Qwen3.6-Plus";
config.modelConfig.formatType = ModelFormatType::OPENAI;

// 4. 配置多会话设置
config.dataBasePath = "./data";
config.maxConcurrentSessions = 3; // 全局并发门控（0 = 无限制）
config.defaultTools = rm.GetAvailableTools();

// 5. 配置上下文引擎
config.contextConfig.sessionId = kDefaultSessionId;
config.contextConfig.storageType = ContextConfig::StorageType::JSON_FILE;

// 6. 初始化 SessionManager（内部创建单个共享 Agent）
InitSessionManager(config);

// 7. 通过 SessionManager 调用，带会话 ID 和流式回调
auto result = GetSessionManager().Invoke(
    "user-session-001",
    "你好，你能做什么？",
    [](const std::string& resp) {
        std::cout << resp << std::flush;
    }
);

if (!result.success) {
    std::cerr << "错误: " << result.errorMessage << std::endl;
}
```

## 测试

```bash
# 构建测试
cmake --build build-linux --target unittest

# 运行测试
./build-linux/unittest
```

## 项目结构

```
jiuwen-lite/
├── include/              # 公共 API 头文件（库接口）
│   ├── agent.h           # Agent 类（会话驱动）
│   ├── session_manager.h # SessionManager 单例
│   ├── types.h           # 配置结构体、SessionConfig、DreamConfig 等
│   └── resource_manager.h
├── src/                  # 库实现
│   ├── core/            # Agent 和 worker 基类
│   │   ├── agent.cpp
│   │   ├── dream_processor.h/cpp   # 后台记忆整合
│   │   └── history_store.h/cpp     # 交互历史跟踪
│   ├── session/         # 会话管理
│   │   └── session_manager.cpp
│   ├── channels/        # 通信通道（Web、Feishu 等）
│   │   ├── channel_manager.h/cpp   # 通道配置 UI
│   │   └── feishu_channel.h/cpp    # 飞书/Lark WebSocket 机器人
│   ├── web/             # Web API 和 UI
│   │   ├── web_api.h/cpp           # 带 SSE 流式传输的 HTTP 服务器
│   │   └── public/index.html       # 内置 Web UI
│   ├── workers/         # ReAct、Plan-and-Execute、Workflow 工作器
│   ├── models/          # OpenAI、Anthropic 和自定义模型实现
│   ├── tools/           # 内置工具（框架）和 MCP 集成
│   │   └── builtin_tools/
│   ├── protocol/        # MCP JSON-RPC 客户端
│   ├── context_engine/  # 上下文存储后端
│   │   ├── storage_base.h/cpp # 通用存储逻辑
│   │   ├── json_storage.h/cpp # JSON 文件存储（默认）
│   │   └── db_storage.h/cpp   # SQLite 存储
│   ├── skills/          # 技能加载和管理
│   └── utils/           # 日志、数据目录、工具解析工具
├── examples/
│   └── jiuwenClaw/      # 示例应用（如何使用框架）
│       ├── main.cpp
│       ├── cron_watcher.h/cpp  # 独立的定时任务模块
│       ├── heartbeat_manager.h/cpp
│       ├── templates/          # 提示词模板
│       └── tools/              # 演示专用工具
├── doc/                 # 文档
│   ├── en/              # 英文文档
│   └── cn/              # 中文文档
├── release_notes/       # 发布说明
├── unittest/            # 单元测试
├── testcases/           # 功能测试
├── libs/                # 第三方共享库（git 忽略）
└── dist/                # 构建输出（git 忽略）
    ├── linux/
    │   ├── libagent_framework.so
    │   └── jiuwenClaw
    └── windows/
        ├── agent_framework.dll
        └── jiuwenClaw.exe
```

## 许可证

Apache 2.0

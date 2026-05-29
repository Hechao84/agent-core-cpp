# jiuwen-lite 中文文档

一个轻量级、模块化的 C++ AI Agent 框架，用于构建推理型智能体。它以共享库形式提供多种 LLM 提供商支持、
Agent 工作模式、内置工具、MCP 集成、技能管理以及基于会话的多租户运行时。**传输层**（HTTP、Web UI、IM 机器人）
有意不包含在核心库中，由 `examples/` 下的参考应用提供。

## 构建产物

构建完成后，将生成以下产物：

| 产物 | Linux 路径 | Windows 路径 | 描述 |
|------|-----------|--------------|------|
| **核心库** | `dist/linux/libagent_framework.so` | `dist/windows/agent_framework.dll` | 框架核心共享库 |
| **头文件** | `include/` | `include/` | 集成库所需的公共 API 头文件 |
| **演示应用** | `dist/linux/jiuwenClaw` | `dist/windows/jiuwenClaw.exe` | 参考演示应用（详见 `examples/jiuwenClaw/README.md`） |

## 功能特性

### Agent 工作模式
- **ReAct** —— 迭代式推理与行动循环
- **Plan-and-Execute** —— 先生成计划，再依次执行
- **Workflow** —— 基于节点的流水线执行 *(规划中)*

### LLM 支持
- OpenAI 兼容 API 格式
- Anthropic API 格式
- **基于 Provider 的自定义模型扩展** —— 通过
  `ResourceManager::RegisterModel(provider, factory)` 注册厂商实现，并通过
  `ModelConfig::provider` 选用。适用于"大体兼容 OpenAI、但消息角色或流式细节需要定制"的后端。

### 内置工具（12 个）
| 工具 | 用途 |
|------|------|
| `time_info` | 获取当前时间/日期 |
| `web_search` | 网络搜索 |
| `web_fetcher` | 获取网页内容 |
| `read_file` | 读取文件内容 |
| `write_file` | 写入文件 |
| `edit_file` | 原地编辑文件 |
| `list_dir` | 列出目录内容 |
| `glob` | 文件路径模式匹配 |
| `grep` | 文件内容搜索 |
| `exec` | 执行 Shell 命令 |
| `skill_search` | 搜索并加载技能指令 |
| `file_state` | 跟踪文件状态变化 |

### MCP 集成
- 支持 STDIO、SSE 和 Streamable HTTP 传输的 Model Context Protocol
- 可连接外部 MCP 服务器以扩展工具能力

### 上下文引擎
- **仅内存** —— 临时存储
- **JSON 文件** —— 将消息持久化为 `.json` 文件（默认）
- **数据库** —— 基于 SQLite 的持久化存储
- 通用基类 `ContextStorageBase` 共享逻辑，配合多个具体后端
- 自动 token 估算与上下文窗口管理

### 会话管理器
- **单一共享 Agent** —— `SessionManager` 拥有唯一的 `Agent`，并按会话注入对应的 `ContextEngine`
- **每会话串行锁** —— 串行化同一会话的调用以保证线程安全
- **全局并发门控** —— `AgentConfig::maxConcurrentSessions` 限制并发会话调用数
- **通道路由** —— `ChannelMessage` + `MakeSessionKey(channel, chatId)` 可从任意传输
  （websocket、feishu、telegram、cli ……）派生会话键
- **保留会话 ID**：
  - `__DEFAULT__` —— 未显式指定 `sessionId` 时使用的默认会话
  - `__HEARTBEAT__` —— 周期性后台任务专用
  - `__CRON__` —— 定时（cron 式）任务专用

### Dream 记忆整合
- **后台整合** —— 空闲会话触发 Dream 处理器执行记忆整合
- **历史存储** —— 基于游标记录交互历史
- **两阶段处理** —— 分析历史、提取关键事实、更新长期记忆
- **自动化** —— 无需手动调用 `UpdateMemory()`；在
  `ContextConfig::idleConsolidationSeconds` 配置的空闲超时后自动触发

### 技能系统
- 从目录结构加载技能，支持 `SKILL.md` 与 YAML frontmatter
- **渐进式披露** —— 元数据始终可用，完整指令按需加载

### 传输层
核心库与传输层解耦。参考适配器（HTTP REST + SSE + Web UI、飞书 WebSocket 机器人）作为独立静态库
（`jiuwenClaw_http_server_adapter`、`jiuwenClaw_feishu_adapter`）由 `examples/jiuwenClaw` 提供。
下游用户既可以直接复用，也可以基于 `SessionManager` 自行编写适配器。

## 系统要求

### 系统依赖
- C++17 编译器（Linux：GCC/Clang，Windows：MSVC）
- CMake >= 3.15
- libcurl（带 SSL 支持）
- pthread（仅 Linux）
- OpenSSL（可选；用于 jiuwenClaw 飞书适配器的 `wss://` WebSocket 支持）

### 第三方依赖（由脚本自动构建）
- nlohmann/json v3.11.3（仅头文件）
- SQLite3 3.45.3（用于数据库上下文存储）
- cpp-httplib（仅头文件，jiuwenClaw HTTP 适配器使用）

## 环境配置

### Linux
**Ubuntu / Debian:**
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libcurl4-openssl-dev libssl-dev pkg-config
```
**CentOS / RHEL:**
```bash
sudo yum groupinstall "Development Tools"
sudo yum install -y cmake libcurl-devel openssl-devel pkg-config
```

### Windows
Windows 构建脚本能为你处理大部分复杂性，但前提是环境配置正确。

1. **前置条件**
   - **Visual Studio 2019/2022/2026（社区版或更高）**
   - **使用 C++ 的桌面开发工作负载**（必须包含 MSVC 工具与 Windows SDK）

2. **环境变量与 vcpkg**
   - 项目使用 **vcpkg** 清单模式管理依赖（如 `libcurl`）。请确保根目录的 `vcpkg.json` 已纳入 git 管理。
   - 确保环境变量 **`VCPKG_ROOT`** 指向你的 vcpkg 安装目录（例如 `C:\vcpkg` 或 `D:\tools\vcpkg`）。
   - *若尚未安装 vcpkg，请克隆它并运行 `bootstrap-vcpkg.bat`。*

3. **构建**
   - **重要：** 请务必在 **"VS 的 x64 Native Tools 命令提示"**（64位系统）（或 **"Developer Command Prompt for VS"** - 32位系统）
     中运行构建脚本，因为脚本需要 `cl.exe` 和标准 MSVC 环境变量。

## 构建

### Linux
```bash
./build_linux.sh
```

### Windows
```cmd
build_windows.bat
```
*（运行脚本前请确保 `VCPKG_ROOT` 已定义。）*

两个脚本都会：
1. 构建第三方依赖（nlohmann/json、SQLite3）
2. 以 Release 模式配置 CMake
3. 构建共享库与 `jiuwenClaw` 演示应用
4. 将输出打包到 `dist/<platform>/`

## 在你的项目中使用该库

### CMake 集成

```cmake
target_link_libraries(your_app PRIVATE agent_framework)
target_include_directories(your_app PRIVATE /path/to/jiuwen-lite/include)
```

### 最小示例

```cpp
#include "include/agent.h"
#include "include/resource_manager.h"
#include "include/session_manager.h"

using namespace jiuwen;

// 1. 获取资源管理器单例（内置工具和模型已自动注册）
auto& rm = ResourceManager::GetInstance();

// 2.（可选）为厂商专属后端注册自定义模型 provider
// rm.RegisterModel("my_vendor", [](const ModelConfig& cfg) {
//     return std::make_unique<MyVendorModel>(cfg);
// });

// 3. 配置 Agent
AgentConfig config;
config.id = "my-agent";
config.name = "My Agent";
config.mode = AgentWorkMode::REACT;
config.maxIterations = 5;

// 模型配置
config.modelConfig.baseUrl = "http://your-llm-endpoint/v1";
config.modelConfig.apiKey = "<your-api-key>";
config.modelConfig.modelName = "<your-model-name>";
config.modelConfig.formatType = ModelFormatType::OPENAI;
// config.modelConfig.provider = "my_vendor"; // 使用自定义 provider 时

// 会话相关设置
config.dataBasePath = "./data";
config.maxConcurrentSessions = 3;          // 0 = 无限制
config.defaultTools = rm.GetAvailableTools();

// 上下文引擎（每会话存储路径由 SessionManager 自动生成）
config.contextConfig.sessionId = kDefaultSessionId;
config.contextConfig.storageType = ContextConfig::StorageType::JSON_FILE;

// 4. 初始化 SessionManager（内部创建唯一共享 Agent）
InitSessionManager(config);

// 5. 通过 SessionManager 调用，传入会话 ID 与流式回调
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

如需完整可运行的应用 —— 包含 CLI、HTTP 服务器（带 Web UI）、飞书机器人、心跳与定时任务 ——
请查看 [`examples/jiuwenClaw/`](../../examples/jiuwenClaw/README.md)
（[中文版](../../examples/jiuwenClaw/doc/cn/README.md)）。

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
├── include/                  # 公共 API 头文件
│   ├── agent.h               # Agent 类（会话驱动）
│   ├── agent_export.h        # 跨平台 DLL 导出宏
│   ├── model.h               # Model 基类
│   ├── resource_manager.h    # 全局工厂注册表
│   ├── session_manager.h     # SessionManager 单例
│   ├── tool.h                # Tool 基类
│   └── types.h               # 配置结构体、枚举、ChannelMessage 等
├── src/                      # 核心库实现
│   ├── core/                 # Agent 核心
│   │   ├── agent.cpp
│   │   ├── agent_worker.{h,cpp}
│   │   ├── dream_processor.{h,cpp}   # 后台记忆整合
│   │   └── history_store.{h,cpp}     # 交互历史追踪
│   ├── session/              # SessionManager 实现
│   ├── resource_manager/     # ResourceManager 实现
│   ├── workers/              # ReAct / Plan-and-Execute / Workflow 工作器
│   ├── models/               # OpenAI、Anthropic 模型实现
│   ├── tools/                # Tool 基类 + MCP 工具
│   │   └── builtin_tools/    # 12 个内置工具
│   ├── protocol/             # MCP JSON-RPC 客户端
│   ├── context_engine/       # 上下文存储后端（内存 / JSON / SQLite）
│   ├── skills/               # 技能加载与管理
│   └── utils/                # 日志、编码、提示词工具、工具解析
├── examples/
│   └── jiuwenClaw/           # 参考演示应用（详见其自身 README）
├── doc/                      # 文档
│   ├── en/                   # 英文文档
│   └── cn/                   # 中文文档
├── release_notes/            # 发布说明
├── unittest/                 # 单元测试
├── testcases/                # 功能测试
├── third_party/              # 第三方源码与头文件
├── libs/                     # 第三方共享库（git 忽略）
└── dist/                     # 构建输出（git 忽略）
    ├── linux/
    │   ├── libagent_framework.so
    │   └── jiuwenClaw
    └── windows/
        ├── agent_framework.dll
        └── jiuwenClaw.exe
```

## 文档

- **English**: 参见 `doc/en/` 目录及 [`examples/jiuwenClaw/README.md`](../../examples/jiuwenClaw/README.md)
- **中文**: 本文件，以及 [`examples/jiuwenClaw/doc/cn/README.md`](../../examples/jiuwenClaw/doc/cn/README.md)

## 许可证

Apache 2.0

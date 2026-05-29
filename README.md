# jiuwen-lite

A lightweight, modular C++ AI agent framework for building reasoning agents. Provides a shared library with
multiple LLM providers, agent work modes, built-in tools, MCP integration, skill management, and a session-based
multi-tenant runtime. Transport layers (HTTP, Web UI, IM bots) are intentionally **not** part of the core library
and are provided by reference applications under `examples/`.

## Artifacts

After building, the following artifacts are produced:

| Artifact | Path (Linux) | Path (Windows) | Description |
|----------|-------------|----------------|-------------|
| **Library** | `dist/linux/libagent_framework.so` | `dist/windows/agent_framework.dll` | Core framework shared library |
| **Headers** | `include/` | `include/` | Public API headers for integrating the library |
| **Demo App** | `dist/linux/jiuwenClaw` | `dist/windows/jiuwenClaw.exe` | Reference demo application (see `examples/jiuwenClaw/README.md`) |

## Features

### Agent Work Modes
- **ReAct** — Iterative reasoning and acting loop
- **Plan-and-Execute** — Generate a plan, then execute each step sequentially
- **Workflow** — Node-based pipeline execution with configurable steps *(planned)*

### LLM Support
- OpenAI-compatible API format
- Anthropic API format
- **Provider-based custom model extension** — Register vendor-specific implementations via
  `ResourceManager::RegisterModel(provider, factory)` and select them by setting `ModelConfig::provider`.
  Useful when a backend is mostly OpenAI-compatible but needs custom handling (message roles, streaming, etc.).

### Built-in Tools (12)
| Tool | Purpose |
|------|---------|
| `time_info` | Get current time/date |
| `web_search` | Search the web |
| `web_fetcher` | Fetch web page content |
| `read_file` | Read file contents |
| `write_file` | Write to files |
| `edit_file` | Edit files in place |
| `list_dir` | List directory contents |
| `glob` | File pattern matching |
| `grep` | Content search in files |
| `exec` | Execute shell commands |
| `skill_search` | Search and load skill instructions |
| `file_state` | Track file state changes |

### MCP Integration
- Model Context Protocol support with STDIO, SSE, and Streamable HTTP transports
- Connect to external MCP servers for extended tool capabilities

### Context Engine
- **Memory Only** — Ephemeral storage
- **JSON File** — Persist messages as `.json` files (default)
- **Database** — SQLite-backed persistent storage
- Common base class (`ContextStorageBase`) for shared logic, with specialized backends
- Automatic token estimation and context window management

### Session Manager
- **Single Shared Agent** — `SessionManager` owns one `Agent` instance and routes a per-session `ContextEngine` to it
- **Per-Session Locking** — Serializes same-session calls for thread safety
- **Global Concurrency Gate** — `AgentConfig::maxConcurrentSessions` limits concurrent session invocations
- **Channel Routing** — `ChannelMessage` + `MakeSessionKey(channel, chatId)` derive a session key from
  any transport (websocket, feishu, telegram, cli, ...)
- **Reserved Session IDs**:
  - `__DEFAULT__` — Default session used when no `sessionId` is explicitly supplied
  - `__HEARTBEAT__` — Reserved for periodic background tasks
  - `__CRON__` — Reserved for scheduled (cron-like) tasks

### Dream Memory Consolidation
- **Background Consolidation** — Idle sessions trigger the Dream processor to consolidate memory
- **History Store** — Records interaction history with cursor-based tracking
- **Two-Phase Processing** — Analyze history, extract key facts, update long-term memory
- **Automatic** — No manual `UpdateMemory()` needed; happens after a configurable idle timeout
  (`ContextConfig::idleConsolidationSeconds`)

### Skill System
- Load skills from a directory structure with `SKILL.md` and YAML frontmatter
- **Progressive disclosure** — Metadata always available, full instructions loaded on demand

### Transport Layers
The core library is transport-agnostic. Reference adapters (HTTP REST + SSE + Web UI, Feishu WebSocket bot)
are provided by `examples/jiuwenClaw` as separate static libraries
(`jiuwenClaw_http_server_adapter`, `jiuwenClaw_feishu_adapter`). Downstream users may either reuse them as-is
or write their own adapters against `SessionManager`.

## Requirements

### System Dependencies
- C++17 compiler (GCC/Clang on Linux, MSVC on Windows)
- CMake >= 3.15
- libcurl (with SSL support)
- pthread (Linux only)
- OpenSSL (optional; enables `wss://` WebSocket support in the jiuwenClaw Feishu adapter)

### Third-Party (auto-built by scripts)
- nlohmann/json v3.11.3 (header-only)
- SQLite3 3.45.3 (for DB context storage)
- cpp-httplib (header-only, used by the jiuwenClaw HTTP adapter)

## Environment Setup

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
The Windows build scripts handle most of the complexity for you, provided your environment is set up correctly.

1. **Prerequisites**
   - **Visual Studio 2019/2022/2026 (Community or higher)**
   - **Desktop development with C++ workload** (must include MSVC tools and Windows SDK)

2. **Environment Variables & vcpkg**
   - The project uses **vcpkg** in manifest mode to manage dependencies (e.g. `libcurl`). Keep
     `vcpkg.json` at the repo root tracked in git.
   - Ensure the environment variable **`VCPKG_ROOT`** points at your vcpkg installation directory
     (e.g. `C:\vcpkg` or `D:\tools\vcpkg`).
   - *If you do not have vcpkg yet, clone it and run `bootstrap-vcpkg.bat`.*

3. **Building**
   - **IMPORTANT:** Always run the build script from an **"x64 Native Tools Command Prompt for VS"** (for X64)
     (or **"Developer Command Prompt for VS"** for X86) because the script needs `cl.exe` and the standard
     MSVC environment variables.

## Building

### Linux
```bash
./build_linux.sh
```

### Windows
```cmd
build_windows.bat
```
*(Ensure `VCPKG_ROOT` is defined before running the script.)*

Both scripts:
1. Build third-party dependencies (nlohmann/json, SQLite3)
2. Configure CMake in Release mode
3. Build the shared library and the `jiuwenClaw` demo application
4. Package outputs to `dist/<platform>/`

## Using the Library in Your Project

### CMake Integration

```cmake
target_link_libraries(your_app PRIVATE agent_framework)
target_include_directories(your_app PRIVATE /path/to/jiuwen-lite/include)
```

### Minimal Example

```cpp
#include "include/agent.h"
#include "include/resource_manager.h"
#include "include/session_manager.h"

using namespace jiuwen;

// 1. Get the resource manager singleton (built-in tools and models are auto-registered)
auto& rm = ResourceManager::GetInstance();

// 2. (Optional) Register a custom model provider for a vendor-specific backend
// rm.RegisterModel("my_vendor", [](const ModelConfig& cfg) {
//     return std::make_unique<MyVendorModel>(cfg);
// });

// 3. Configure the agent
AgentConfig config;
config.id = "my-agent";
config.name = "My Agent";
config.mode = AgentWorkMode::REACT;
config.maxIterations = 5;

// Model configuration
config.modelConfig.baseUrl = "http://your-llm-endpoint/v1";
config.modelConfig.apiKey = "<your-api-key>";
config.modelConfig.modelName = "<your-model-name>";
config.modelConfig.formatType = ModelFormatType::OPENAI;
// config.modelConfig.provider = "my_vendor"; // when using a custom provider

// Session-related settings
config.dataBasePath = "./data";
config.maxConcurrentSessions = 3;          // 0 = unlimited
config.defaultTools = rm.GetAvailableTools();

// Context engine (per-session storage paths are derived by SessionManager)
config.contextConfig.sessionId = kDefaultSessionId;
config.contextConfig.storageType = ContextConfig::StorageType::JSON_FILE;

// 4. Initialize SessionManager (creates a single shared Agent internally)
InitSessionManager(config);

// 5. Invoke via SessionManager with a session ID and streaming callback
auto result = GetSessionManager().Invoke(
    "user-session-001",
    "Hello, what can you do?",
    [](const std::string& chunk) {
        std::cout << chunk << std::flush;
    }
);

if (!result.success) {
    std::cerr << "Error: " << result.errorMessage << std::endl;
}
```

For a complete working application — including CLI, HTTP server with Web UI, Feishu bot, heartbeat and cron
tasks — see [`examples/jiuwenClaw/`](examples/jiuwenClaw/README.md).

## Testing

```bash
# Build tests
cmake --build build-linux --target unittest

# Run tests
./build-linux/unittest
```

## Project Structure

```
jiuwen-lite/
├── include/                  # Public API headers
│   ├── agent.h               # Agent class (session-driven)
│   ├── agent_export.h        # Cross-platform DLL export macro
│   ├── model.h               # Model base class
│   ├── resource_manager.h    # Global factory registry
│   ├── session_manager.h     # SessionManager singleton
│   ├── tool.h                # Tool base class
│   └── types.h               # Config structs, enums, ChannelMessage, etc.
├── src/                      # Core library implementation
│   ├── core/                 # Agent core
│   │   ├── agent.cpp
│   │   ├── agent_worker.{h,cpp}
│   │   ├── dream_processor.{h,cpp}   # Background memory consolidation
│   │   └── history_store.{h,cpp}     # Interaction history tracking
│   ├── session/              # SessionManager implementation
│   ├── resource_manager/     # ResourceManager implementation
│   ├── workers/              # ReAct, Plan-and-Execute, Workflow workers
│   ├── models/               # OpenAI and Anthropic model implementations
│   ├── tools/                # Tool base + MCP tool
│   │   └── builtin_tools/    # 12 built-in tools
│   ├── protocol/             # MCP JSON-RPC client
│   ├── context_engine/       # Context storage backends (memory / JSON / SQLite)
│   ├── skills/               # Skill loading and management
│   └── utils/                # Logging, encoding, prompt utilities, tool parsing
├── examples/
│   └── jiuwenClaw/           # Reference demo app (see its own README for details)
├── doc/                      # Documentation
│   ├── en/                   # English documentation
│   └── cn/                   # Chinese documentation
├── release_notes/            # Release notes
├── unittest/                 # Unit tests
├── testcases/                # Functional tests
├── third_party/              # Third-party sources / headers
├── libs/                     # Third-party shared libraries (gitignored)
└── dist/                     # Build output (gitignored)
    ├── linux/
    │   ├── libagent_framework.so
    │   └── jiuwenClaw
    └── windows/
        ├── agent_framework.dll
        └── jiuwenClaw.exe
```

## Documentation

- **English**: See `doc/en/` directory and [`examples/jiuwenClaw/README.md`](examples/jiuwenClaw/README.md)
- **中文**: 请查看 [`doc/cn/`](doc/cn/README.md) 目录及 [`examples/jiuwenClaw/doc/cn/README.md`](examples/jiuwenClaw/doc/cn/README.md)

## License

Apache 2.0

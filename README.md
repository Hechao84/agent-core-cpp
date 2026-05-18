# jiuwen-lite

A lightweight, modular C++ AI agent framework library for building reasoning agents. Provides shared library with multiple LLM providers, agent work modes, built-in tools, MCP integration, and skill management.

## Artifacts

After building, the following artifacts are produced:

| Artifact | Path (Linux) | Path (Windows) | Description |
|----------|-------------|----------------|-------------|
| **Library** | `dist/linux/libagent_framework.so` | `dist/windows/agent_framework.dll` | Core framework shared library |
| **Headers** | `include/` | `include/` | Public API headers for integrating the library |
| **Demo App** | `dist/linux/jiuwenClaw` | `dist/windows/jiuwenClaw.exe` | Sample application showing how to use the framework |

## Features

### Agent Work Modes
- **ReAct** — Iterative reasoning and acting loop
- **Plan-and-Execute** — Generate a plan, then execute each step sequentially
- **Workflow** — Node-based pipeline execution with configurable steps *(planned)*

### LLM Support
- OpenAI-compatible API format
- Anthropic API format

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

### jiuwenClaw Demo-specific Tools (3)
| Tool | Purpose |
|------|---------|
| `cron` | Schedule, list, and remove reminders |
| `notify` | Cross-platform desktop notifications |
| `notebook_edit` | Edit Jupyter notebooks |

### MCP Integration
- Model Context Protocol support with STDIO, SSE, and Streamable HTTP transports
- Connect to external MCP servers for extended tool capabilities

### Context Engine
- **Memory Only** — Ephemeral storage
- **JSON File** — Persist messages as `.json` files (default)
- **Database** — SQLite-backed persistent storage (implemented)
- Common base class (`ContextStorageBase`) for shared logic, with specialized backends
- Automatic token estimation and context window management

### Session Manager
- **Multi-Session Support** — Isolated conversation contexts per session
- **Per-Session Locking** — Serializes same-session calls for thread safety
- **Concurrency Gate** — Global limit on concurrent session invocations (`maxConcurrentSessions`)
- **Channel Routing** — Auto-derives session key from channel type (websocket, feishu, telegram, cli) + chatId
- **Reserved Sessions** — Built-in sessions for internal tasks (`__HEARTBEAT__`, `__CRON__`, `__UNIFIED__`)

### Dream Memory Consolidation
- **Background Consolidation** — Idle sessions trigger Dream processor to consolidate memory
- **History Store** — Records interaction history with cursor-based tracking
- **Two-Phase Processing** — Analyze history, extract key facts, update long-term memory
- **Automatic** — No manual `UpdateMemory()` needed; happens after configurable idle timeout

### Skill System
- Load skills from directory structure with `SKILL.md` and YAML frontmatter
- Progressive disclosure: metadata always available, full instructions loaded on-demand

## Requirements

### System Dependencies
- C++17 compiler (GCC/Clang on Linux, MSVC on Windows)
- CMake >= 3.15
- libcurl (with SSL support)
- pthread (Linux only)

### Third-Party (auto-built by scripts)
- nlohmann/json v3.11.3 (header-only)
- SQLite3 3.45.3 (for DB context storage)

## Environment Setup

Before building, ensure your development environment meets the requirements below.

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
The Windows build scripts are designed to handle most of the complexity for you, provided your environment is set up correctly.

1.  **Prerequisites**
    - **Visual Studio 2019/2022/2026 (Community or higher)**.
    - **Desktop development with C++ workload** (Must include MSVC tools and Windows SDK).

2.  **Environment Variables & vcpkg**
    - The project uses **vcpkg** in manifest mode to automatically manage dependencies (like `libcurl`). Ensure `vcpkg.json` at the root is tracked in git.
    - Ensure the environment variable **`VCPKG_ROOT`** is set to your vcpkg installation directory (e.g., `C:\vcpkg` or `D:\tools\vcpkg`).
    - *Note: If you do not have vcpkg installed yet, clone it and run `bootstrap-vcpkg.bat`.*

3.  **Building**
    - **IMPORTANT:** Always run the build script from a **"Developer Command Prompt for VS"** (or VS20xx **"x64 Native Tools Command Prompt"**) because the script requires `cl.exe` and standard MSVC environment variables.

## Building

### Linux

```bash
./build_linux.sh
```

### Windows
```cmd
build_windows.bat
```
*(Note: Ensure `VCPKG_ROOT` is defined before running the script.)*

Both scripts:
1. Build third-party dependencies (nlohmann/json, SQLite3)
2. Configure CMake with Release mode
3. Build the shared library and demo application
4. Package outputs to `dist/<platform>/`

## Running the Demo

### Linux
```bash
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw
```

### Windows
```powershell
$env:PATH = "$PWD\dist\windows;$env:PATH"
.\dist\windows\jiuwenClaw.exe
```

## Using the Library in Your Project

### CMake Integration

```cmake
# Link to the framework library
target_link_libraries(your_app PRIVATE agent_framework)
target_include_directories(your_app PRIVATE /path/to/jiuwen-lite/include)
```

### Example: Creating an Agent with SessionManager

See `examples/jiuwenClaw/main.cpp` for a complete working example. The core usage pattern is:

```cpp
#include "agent.h"
#include "resource_manager"
#include "session_manager.h"

using namespace jiuwen;

// 1. Get the resource manager singleton
auto& rm = ResourceManager::GetInstance();

// 2. Configure the agent
AgentConfig config;
config.id = "my-agent";
config.name = "My Agent";
config.mode = AgentWorkMode::REACT;
config.maxIterations = 5;

// 3. Configure the model
config.modelConfig.baseUrl = "http://your-llm-endpoint/v1";
config.modelConfig.apiKey = "your-api-key";
config.modelConfig.modelName = "Qwen3.6-Plus";
config.modelConfig.formatType = ModelFormatType::OPENAI;

// 4. Configure multi-session settings
config.dataBasePath = "./data";
config.maxConcurrentSessions = 3; // Global concurrency gate (0 = unlimited)
config.defaultTools = rm.GetAvailableTools();

// 5. Configure context engine
config.contextConfig.sessionId = kDefaultSessionId;
config.contextConfig.storageType = ContextConfig::StorageType::JSON_FILE;

// 6. Initialize SessionManager (creates single shared Agent internally)
InitSessionManager(config);

// 7. Invoke via SessionManager with session ID and streaming callback
auto result = GetSessionManager().Invoke(
    "user-session-001",
    "Hello, what can you do?",
    [](const std::string& resp) {
        std::cout << resp << std::flush;
    }
);

if (!result.success) {
    std::cerr << "Error: " << result.errorMessage << std::endl;
}
```

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
├── include/              # Public API headers (library interface)
│   ├── agent.h           # Agent class (session-driven)
│   ├── session_manager.h # SessionManager singleton
│   ├── types.h           # Config structs, SessionConfig, DreamConfig, etc.
│   └── resource_manager.h
├── src/                  # Library implementation
│   ├── core/            # Agent and worker base classes
│   │   ├── agent.cpp
│   │   ├── dream_processor.h/cpp   # Background memory consolidation
│   │   └── history_store.h/cpp     # Interaction history tracking
│   ├── session/         # Session management
│   │   └── session_manager.cpp
│   ├── workers/         # ReAct, Plan-and-Execute, Workflow workers
│   ├── models/          # OpenAI and Anthropic model implementations
│   ├── tools/           # Built-in tools (framework) and MCP integration
│   │   └── builtin_tools/
│   ├── protocol/        # MCP JSON-RPC client
│   ├── context_engine/  # Context storage backends
│   │   ├── storage_base.h/cpp # Common storage logic
│   │   ├── json_storage.h/cpp # JSON file storage (default)
│   │   └── db_storage.h/cpp   # SQLite storage
│   ├── skills/          # Skill loading and management
│   └── utils/           # Logging, data directory, tool parsing utilities
├── examples/
│   └── jiuwenClaw/      # Sample application (how to use the framework)
│       ├── main.cpp
│       ├── cron_watcher.h/cpp  # Independent cron task module
│       ├── heartbeat_manager.h/cpp
│       ├── templates/          # Prompt templates
│       └── tools/              # Demo-specific tools
├── unittest/            # Unit tests
├── testcases/           # Functional tests
├── cmake/               # CMake helper modules
├── libs/                # Third-party shared libraries (gitignored)
└── dist/                # Build output (gitignored)
    ├── linux/
    │   ├── libagent_framework.so
    │   └── jiuwenClaw
    └── windows/
        ├── agent_framework.dll
        └── jiuwenClaw.exe
```

## License

Apache 2.0

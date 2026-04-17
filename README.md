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

### Built-in Tools (11)
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

### jiuwenClaw Demo-specific Tools (3)
| Tool | Purpose |
|------|---------|
| `notebook_edit` | Edit Jupyter notebooks |
| `file_state` | Track file state changes |
| `cron` | Scheduled task management |

### MCP Integration
- Model Context Protocol support with STDIO, SSE, and Streamable HTTP transports
- Connect to external MCP servers for extended tool capabilities

### Context Engine
- **Memory Only** — Ephemeral storage
- **Markdown File** — Persist messages as `.md` files (verified)
- **Database** — SQLite-backed persistent storage (implemented)
- Common base class (`ContextStorageBase`) for shared logic, with specialized backends
- Automatic token estimation and context window management

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

The Windows build requires Visual Studio 2022 (Community or higher) and `vcpkg` for dependency management (`libcurl` and `sqlite3` are not always available by default).

1.  **Visual Studio 2022**
    - Download and install [Visual Studio 2022](https://visualstudio.microsoft.com/zh-hans/downloads/).
    - Run **Visual Studio Installer**, click "Modify".
    - Ensure the **Desktop development with C++** workload is selected.
    - Ensure the **MSVC** compiler and **Windows SDK** are checked in the installation details.

2.  **Setup vcpkg**
    [vcpkg](https://learn.microsoft.com/zh-cn/vcpkg) is used to manage C++ libraries:
    ```powershell
    git clone https://github.com/microsoft/vcpkg.git
    cd vcpkg
    .\bootstrap-vcpkg.bat
    
    # Optional: Integrate with Visual Studio for easier usage
    .\vcpkg integrate install
    ```

3.  **Install Dependencies (libcurl & SQLite3)**
    Use vcpkg to install the required libraries (using `x64-windows` triplet):
    ```powershell
    .\vcpkg install curl[ssl,winssl,schannel]:x64-windows sqlite3:x64-windows
    ```
    *Note: `schannel` is recommended over `openssl` for native Windows SSL support.*

4.  **Configure CMAKE_TOOLCHAIN_FILE**
    You must tell CMake to use vcpkg. You can do this by adding `-DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake` to your build command or environment variables.

## Building

### Linux

```bash
./build_linux.sh
```

### Windows

```powershell
# Ensure you are in a Developer Command Prompt or PowerShell with vcvarsall loaded
.\build_windows.bat
```

*Note: If you use vcpkg (as recommended above), you may need to pass the toolchain file to CMake. You can modify `build_windows.bat` or run CMake manually with `-DCMAKE_TOOLCHAIN_FILE=<vcpkg_root>/scripts/buildsystems/vcpkg.cmake`.*

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

### Example: Creating an Agent

See `examples/jiuwenClaw/main.cpp` for a complete working example. The core usage pattern is:

```cpp
#include "agent.h"
#include "resource_manager.h"

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

// 4. Create the agent
Agent agent(config);

// 5. Add tools
agent.AddTools(rm.GetAvailableTools());

// 6. Invoke with a callback for streaming responses
agent.Invoke("Hello, what can you do?", [](const std::string& resp) {
    std::cout << resp;
});
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
├── src/                  # Library implementation
│   ├── core/            # Agent and worker base classes
│   ├── workers/         # ReAct, Plan-and-Execute, Workflow workers
│   ├── models/          # OpenAI and Anthropic model implementations
│   ├── tools/           # Built-in tools (framework) and MCP integration
│   ├── protocol/        # MCP JSON-RPC client
│   ├── context_engine/  # Context storage backends
│   │   ├── storage_base.h/cpp # Common storage logic
│   │   ├── md_storage.h/cpp   # Markdown file storage
│   │   └── db_storage.h/cpp   # SQLite storage
│   ├── skills/          # Skill loading and management
│   └── utils/           # Logging and utilities
├── examples/
│   └── jiuwenClaw/      # Sample application (how to use the framework)
│       ├── main.cpp
│       └── tools/       # Demo-specific tools
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

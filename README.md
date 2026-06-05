# jiuwen-lite

A lightweight, modular C++ AI agent framework for building tool-using reasoning agents. The core is delivered as a
shared library and provides model adapters, ReAct execution, native function-calling, MCP integration, skills,
per-session context, memory consolidation, and a multi-session runtime. Transport layers such as HTTP, Web UI, and
IM bots are intentionally kept outside the core library and are implemented by reference applications under
`examples/`.

## Artifacts

After building, the following artifacts are produced:

| Artifact | Path (Linux) | Path (Windows) | Description |
|----------|--------------|----------------|-------------|
| **Library** | `dist/linux/libagent_framework.so` | `dist/windows/agent_framework.dll` | Core framework shared library |
| **Headers** | `include/` | `include/` | Public API headers for integrating the library |
| **Demo App** | `dist/linux/jiuwenClaw` | `dist/windows/jiuwenClaw.exe` | Reference application |

## Features

### Agent Work Modes

- **ReAct** — Production execution mode in this build. Supports iterative reasoning, native tool calls, fallback
  prompt-parsed tool calls, observations, and final answers.
- **Plan-and-Execute / Workflow** — Not implemented yet; support may be considered in future releases.

### LLM Support

- OpenAI-compatible API format
- Anthropic API format
- Native function-calling protocol for OpenAI-style `tools/tool_calls` and Anthropic `tool_use/tool_result`
- Prompt-only fallback mode via `ModelConfig::useNativeFunctionCalling = false`
- Extended model parameters through `ModelConfig::extraParams`, including common OpenAI request fields such as
  `max_tokens`, `temperature`, `top_p`, `presence_penalty`, `frequency_penalty`, and `seed`
- Provider-based custom model extension via `ResourceManager::RegisterModel(provider, factory)` and
  `ModelConfig::provider`

### Built-in Tools

The framework includes 12 stateless built-in tools and 6 session-scoped tools.

| Tool | Scope | Purpose |
|------|-------|---------|
| `time_info` | Stateless | Get current time/date |
| `web_search` | Stateless | Search the web, with fallback between supported engines |
| `web_fetcher` | Stateless | Fetch web page content |
| `read_file` | Stateless | Read file contents |
| `write_file` | Stateless | Write to files |
| `edit_file` | Stateless | Edit files in place |
| `list_dir` | Stateless | List directory contents |
| `glob` | Stateless | File pattern matching |
| `grep` | Stateless | Content search in files |
| `exec` | Stateless | Execute shell commands |
| `skill_search` | Stateless | Search and load skill instructions |
| `file_state` | Stateless | Track file state changes |
| `todo_create` | Session | Create the current session's task list |
| `todo_complete` | Session | Mark a todo item complete with a result |
| `todo_insert` | Session | Insert a todo item |
| `todo_remove` | Session | Remove a todo item |
| `todo_list` | Session | List current todo items |
| `ask_user` | Session | Ask the application/user for clarification during a run |

Session-scoped tools are registered through `ResourceManager::RegisterSessionTool` and receive per-session resources
via `ToolBuildContext`.

### MCP Integration

- Model Context Protocol support with STDIO, SSE, and Streamable HTTP transports
- Runtime MCP server registration, unregistration, reconnect, and connected-server introspection
- MCP tools are registered separately from static tools so they can be synchronized when servers change

### Context Engine

- **Memory Only** — Ephemeral storage
- **JSON File** — Persist messages as `.json` files (default)
- **Database** — SQLite-backed persistent storage
- Shared `ContextStorageBase` logic for storage backends
- Automatic token estimation and context window management
- Native persistence of assistant tool calls and tool response metadata

### Session Manager

- **Single Shared Agent** — `SessionManager` owns one live `Agent` and routes each session to its own
  `ContextEngine`
- **Per-Session Locking** — Serializes same-session calls for thread safety
- **Global Concurrency Gate** — `AgentConfig::maxConcurrentSessions` limits concurrent session invocations
- **Channel Routing** — `ChannelMessage` and `MakeSessionKey(channel, chatId)` derive stable session keys from
  transports such as web, Feishu, Telegram, or CLI
- **Hot Reload** — `ReloadAgent` can rebuild the live agent from an updated `AgentConfig`
- **Reserved Session IDs**:
  - `__DEFAULT__` — Default session used when no `sessionId` is explicitly supplied
  - `__HEARTBEAT__` — Reserved for periodic background tasks
  - `__CRON__` — Reserved for scheduled tasks

### Dream Memory Consolidation

- Idle sessions trigger background memory consolidation
- Interaction history is tracked with cursors
- The Dream processor analyzes recent history, extracts key facts, and updates long-term memory
- Consolidation is automatic after `ContextConfig::idleConsolidationSeconds`

### Skill System

- Load skills from directories containing `SKILL.md` with YAML frontmatter
- Progressive disclosure: metadata is available for discovery and full instructions are loaded on demand
- Skills can be listed and inspected through the `Agent` API and the jiuwenClaw HTTP API

### Configuration

- JSON serialization/deserialization for `AgentConfig`
- Persistent override store at `./data/agents.json`
- Optional polling watcher for live reload in applications
- Schema is designed for multiple agents, while the current runtime runs one live agent at a time through `SessionManager`

### Transport Layers

The core library is transport-agnostic. Reference adapters for HTTP REST + SSE + Web UI and Feishu WebSocket bots are
provided by `examples/jiuwenClaw` as separate static libraries. Downstream applications can reuse them or implement
their own adapters against `SessionManager`.

## Requirements

### System Dependencies

- C++17 compiler (GCC/Clang on Linux, MSVC on Windows)
- CMake >= 3.15
- libcurl with SSL support
- pthread on Linux
- OpenSSL optional but recommended for `wss://` WebSocket support in the jiuwenClaw Feishu adapter

### Third-Party Dependencies

- nlohmann/json v3.11.3
- SQLite3 3.45.3
- cpp-httplib for jiuwenClaw HTTP/WebSocket adapters

## Environment Setup

### Linux

Ubuntu / Debian:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libcurl4-openssl-dev libssl-dev pkg-config
```

CentOS / RHEL:

```bash
sudo yum groupinstall "Development Tools"
sudo yum install -y cmake libcurl-devel openssl-devel pkg-config
```

### Windows

1. Install Visual Studio 2019/2022/2026 with the Desktop development with C++ workload.
2. Install vcpkg and set `VCPKG_ROOT` to the vcpkg installation directory.
3. Run `build_windows.bat` from an x64 Native Tools Command Prompt for VS.

## Building

### Linux

```bash
./build_linux.sh
```

### Windows

```cmd
build_windows.bat
```

Both scripts build third-party dependencies, configure CMake in Release mode, build the shared library and
`jiuwenClaw`, then package outputs to `dist/<platform>/`.

## Using the Library in Your Project

### CMake Integration

```cmake
target_link_libraries(your_app PRIVATE agent_framework)
target_include_directories(your_app PRIVATE /path/to/jiuwen-lite/include)
```

### Minimal Example

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
config.contextConfig.idleConsolidationSeconds = 60;

InitSessionManager(config);

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

For a complete application with CLI, HTTP server, Web UI, Feishu bot, channel management, MCP management,
heartbeat, and cron tasks, see [`examples/jiuwenClaw/`](examples/jiuwenClaw/README.md).

## Testing

```bash
# Build tests
cmake --build build-linux --target unittest

# Run tests
./build-linux/unittest
```

Additional smoke tests are available under `tests/` and can be compiled manually when needed.

## Project Structure

```text
jiuwen-lite/
├── include/                  # Public API headers
│   ├── agent.h               # Agent class
│   ├── model.h               # Model base class, ToolCall, ToolSchema
│   ├── resource_manager.h    # Tool/model/MCP registry
│   ├── session_manager.h     # SessionManager singleton
│   ├── tool.h                # Tool base class
│   ├── types.h               # Config structs and runtime types
│   └── config/               # AgentConfig JSON/store/watcher APIs
├── src/                      # Core library implementation
│   ├── core/                 # Agent, worker env, Dream, history, session tools
│   ├── session/              # SessionManager implementation
│   ├── resource_manager/     # ResourceManager implementation
│   ├── workers/              # ReAct worker and worker factory
│   ├── models/               # OpenAI and Anthropic model implementations
│   ├── mcp/                  # MCP client, connection, config manager, MCP tool wrapper
│   ├── tools/                # Tool base and built-in tools
│   ├── context_engine/       # Memory / JSON / SQLite context storage
│   ├── skills/               # Skill loading and management
│   └── utils/                # Logging, encoding, prompt utilities, tool parsing
├── examples/
│   └── jiuwenClaw/           # Reference application
├── doc/                      # Documentation
│   ├── en/
│   └── cn/
├── release_notes/            # Release notes
├── unittest/                 # CMake unit tests
├── tests/                    # Standalone smoke tests
└── third_party/              # Third-party sources / headers
```

## Documentation

- This file
- [`examples/jiuwenClaw/README.md`](examples/jiuwenClaw/README.md)

## License

Apache 2.0

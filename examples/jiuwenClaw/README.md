# jiuwenClaw

`jiuwenClaw` is the reference application built on top of the **jiuwen-lite** framework. It demonstrates how to wire
`SessionManager`, `Agent`, `ResourceManager`, tools, MCP servers, prompt templates, hot-reloadable configuration, and
transport adapters into a complete assistant application.

> For the core framework overview, see the [top-level README](../../README.md).

## Features

- **Interactive CLI** — default run mode with multi-session commands
- **HTTP REST + SSE server** — `/api/chat`, `/api/chat/stream`, session/history APIs, and management APIs
- **Built-in Web UI** — chat, multi-session streaming, channels, skills, agents, and MCP server management
- **Feishu bot integration** — native Feishu/Lark WebSocket channel managed by `channels.json` or Web UI
- **Hot-reloadable agent configuration** — `./data/agents.json`, REST APIs, CLI reload commands, and optional watcher
- **MCP server management** — persistent `./data/mcp_servers.json`, runtime connect/disconnect/reload, and tool sync
- **Dream memory consolidation** — idle sessions distill history into long-term memory
- **Heartbeat tasks** — periodic background tasks through the reserved `__HEARTBEAT__` session
- **Cron tasks** — standalone scheduler through the reserved `__CRON__` session
- **Skill discovery** — list and inspect `SKILL.md` based skills through API/Web UI
- **Application tools** — cron reminders, desktop notifications, and Jupyter notebook editing

## Demo-specific Tools

In addition to the framework tools, `jiuwenClaw` registers three application-level tools:

| Tool | Purpose |
|------|---------|
| `cron` | Schedule, list, and remove reminders |
| `notify` | Cross-platform desktop notifications |
| `notebook_edit` | Edit Jupyter notebooks |

The framework also exposes session-scoped `todo_*` and `ask_user` tools when they are included in the live agent's tool list.

## Transport Adapters

`jiuwenClaw` provides two reference transport adapters as standalone static libraries:

| Adapter | CMake target | What it does |
|---------|--------------|--------------|
| HTTP server | `jiuwenClaw_http_server_adapter` | HTTP REST API, SSE streaming, Web UI, channel APIs, agent APIs, MCP APIs |
| Feishu bot | `jiuwenClaw_feishu_adapter` | Feishu/Lark bot integration over WebSocket |

The core library does not depend on these adapters. You can reuse them, replace them, or implement your own adapters
against `SessionManager`.

## Directory Structure

```text
examples/jiuwenClaw/
├── main.cpp                          # Application entry point, CLI, bootstrapping, reload helpers
├── reserved_sessions.h               # App-layer reserved session constants (__HEARTBEAT__ / __CRON__)
├── heartbeat_manager.{h,cpp}         # Heartbeat module using __HEARTBEAT__
├── cron_watcher.{h,cpp}              # Cron scheduler using __CRON__
├── adapters/
│   ├── http_server/                  # HTTP REST + SSE + Web UI adapter
│   └── feishu/                       # Feishu WebSocket channel adapter
├── channels/                         # Persistent channel registry and runtime service
├── mcp/                              # Persistent MCP server registry
├── tools/                            # Demo-specific tools
├── utils/                            # Encoding, logging, data-dir, string helpers
├── templates/                        # Prompt templates
├── web/                              # Built-in Web UI
└── docs/
    ├── en/
    └── cn/
```

## Build & Run

### Build

```bash
# Linux
./build_linux.sh

# Windows, from an x64 Native Tools Command Prompt for VS
build_windows.bat
```

Build artifacts are placed under `dist/<platform>/`. See the [top-level README](../../README.md) for environment
requirements and vcpkg notes.

### Run CLI

```bash
# Linux
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw

# Windows PowerShell
$env:PATH = "$PWD\dist\windows;$env:PATH"
.\dist\windows\jiuwenClaw.exe
```

### Server Mode

```bash
# CLI + HTTP server + configured channels
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw --server

# Custom host/port
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw --server --host 0.0.0.0 --port 9000

# Daemon mode, no CLI
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw --server --no-cli

# Enable polling-based config reload
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw --server --watch-config
```

Web UI:

```text
http://localhost:8080
```

## Command Line Options

```text
jiuwenClaw [OPTIONS]
  --server       Enable all integrations: HTTP REST API + configured channels
  --port <N>     Set server port (default: 8080)
  --host <IP>    Set server host (default: 127.0.0.1)
  --no-cli       Disable interactive CLI and run as a daemon
  --watch-config Auto-reload agent/channel configuration when files change
  --help         Show help
```

When `--server` is set, enabled channels from `./data/channels.json` are loaded and started.

## CLI Commands

| Command | Description |
|---------|-------------|
| `/exit` | Quit the program |
| `/session <id>` | Switch to a session, creating it if needed |
| `/sessions` | List active sessions |
| `/reload` | Reload `agents.json` and reconcile `channels.json` |
| `/reload agent [<id>]` | Reload one agent config, defaulting to the live agent |
| `/reload channels` | Reconcile running channels with `channels.json` |
| `/config show [<id>]` | Print the effective agent config |

## HTTP API

The server exposes JSON APIs for chat and runtime management:

| API | Purpose |
|-----|---------|
| `GET /api/health` | Health check and session count |
| `GET /api/sessions` | List non-reserved sessions |
| `POST /api/sessions` | Create a session |
| `DELETE /api/sessions/{id}` | Delete a non-reserved session |
| `POST /api/sessions/{id}/cancel` | Request cancellation |
| `GET /api/sessions/{id}/history` | Read session message history |
| `POST /api/chat` | Non-streaming chat |
| `POST /api/chat/stream` | SSE streaming chat |
| `GET /api/tools` | List available tools |
| `GET /api/skills` / `GET /api/skills/{id}` | List and inspect skills |
| `GET/PUT/DELETE /api/agents/{id}` | Manage agent overrides |
| `POST /api/agents/reload` | Reload agent config from disk |
| `GET/POST/PUT/DELETE /api/channels` | Manage transport channels |
| `POST /api/channels/reload` | Reload channel config from disk |
| `GET/POST/PUT/DELETE /api/mcp_servers` | Manage MCP servers |
| `POST /api/mcp_servers/reload` | Reload MCP server config from disk |
| `POST /api/answer` | Resolve a pending `ask_user` request |

`/api/chat/stream` emits typed SSE events including streaming tokens, status updates, tool calls, tool responses,
`ask_user` questions, and final completion.

## Configuration Files

Runtime state is stored under `./data` by default.

| File | Purpose |
|------|---------|
| `agents.json` | Agent override configs. Missing fields fall back to the code default in `BuildAgentConfig()` |
| `channels.json` | Feishu and future channel definitions |
| `mcp_servers.json` | Persistent MCP server definitions |

### Agent Configuration

`agents.json` is multi-agent-ready. Runs one live agent through `SessionManager`, but the store preserves all agent entries.

Example override:

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

### Channel Configuration

Channels are persisted to `./data/channels.json` and can be edited through the Web UI or REST API.

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

### MCP Server Configuration

MCP servers are persisted to `./data/mcp_servers.json`. Supported transports are `streamable-http-client`, `sse`, and
`stdio`.

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

After editing MCP servers, reload through the Web UI, REST API, or restart the app. The live agent synchronizes MCP
tools after server changes.

## Heartbeat

The heartbeat manager periodically inspects `./data/HEARTBEAT.md`. Tasks declared under `## Active Tasks` are evaluated
and dispatched through the `__HEARTBEAT__` session when due.

```markdown
## Active Tasks

- Summarize project updates once a day
- Monitor disk usage every hour
```

If the section is empty or contains only comments, the heartbeat cycle is skipped.

## Cron Tasks

`CronWatcher` is an independent scheduler that invokes the agent through the `__CRON__` session when a trigger fires.

| Type | Description | Parameter |
|------|-------------|-----------|
| `one-time` | Fires once, then is removed | `at` ISO timestamp |
| `recurring` | Fires repeatedly at a fixed interval | `every_seconds` |
| `cron` | Fires according to a cron expression | `cron_expr`, e.g. `"0 9 * * 1-5"` |

Users usually create entries through natural-language interaction, for example:

```text
Remind me to drink water in 10 minutes.
Notify me every weekday at 9am for the morning stand-up.
```

## Prompt Templates

`jiuwenClaw` uses modular prompt templates referenced from `REACT_SYSTEM.md`.

| File | Responsibility | Placeholder |
|------|----------------|-------------|
| `AGENTS.md` | Agent capability list and behavior rules | `{$agents}` |
| `SOUL.md` | Agent personality and communication style | `{$soul}` |
| `USER.md` | User profile and preferences | `{$user}` |
| `TOOLS.md` | Tool usage policies | `{$tools_md}` |
| `MEMORY.md` | Dream memory consolidation prompt | — |
| `REACT_SYSTEM.md` | Master template with runtime context | entry point |

The framework also injects long-term memory and, when enabled, the current session todo list.

## OpenAI-Compatible Model Providers

OpenAI-compatible backends can use the built-in `OpenAIModel` directly. Common request fields such as `max_tokens`, `temperature`, and `top_p` can be supplied through `ModelConfig::extraParams`, so no custom provider is required.

## Building Your Own Agent on jiuwen-lite

1. Create a new application directory under `examples/` or in your own repository.
2. Define prompt templates and assign them in `AgentConfig::promptTemplates`.
3. Register application tools, optional session tools, and optional model providers.
4. Load MCP servers if needed.
5. Initialize `SessionManager` with `InitSessionManager(config)`.
6. Drive interactions through `GetSessionManager().Invoke(sessionId, query, callback)`.
7. Add transport adapters as application-layer code.

## Documentation

- This file
- [Framework README](../../README.md)

## Quick Links

- [Framework README](../../README.md)
- [Release notes](../../release_notes/)

## License

Apache 2.0

# jiuwenClaw

`jiuwenClaw` is the reference demo application built on top of the **jiuwen-lite** framework. It demonstrates
how to wire up the core library (`SessionManager`, `Agent`, `ResourceManager`) with concrete transport adapters
(HTTP REST + SSE + Web UI, Feishu WebSocket bot), application-level features (heartbeat, cron), prompt templates,
and a custom model provider.

> For the core library overview, see the [top-level README](../../README.md).

## Features

- **Multi-session CLI** — switch contexts on the fly with `/session <id>` / `/sessions`
- **ReAct reasoning** — tool-augmented chain-of-thought
- **Dream memory consolidation** — idle sessions automatically distill history into long-term memory
- **Heartbeat tasks** — periodic background tasks executed via the reserved `__HEARTBEAT__` session
- **Cron tasks (`CronWatcher`)** — standalone scheduler using the reserved `__CRON__` session
- **Skill system** — discover and load `SKILL.md` files on demand
- **Custom model provider** — ships an `ArkCodeModel` as an example of vendor-specific OpenAI-compatible handling
- **Dual run modes** — interactive CLI and/or server mode (`--server`) with built-in Web UI
- **Channel persistence** — channel definitions live in `./data/channels.json`, editable via the Web UI

### Demo-specific Tools

In addition to the 12 built-in tools from the core library, `jiuwenClaw` registers three application-level tools:

| Tool | Purpose |
|------|---------|
| `cron` | Schedule, list, and remove reminders |
| `notify` | Cross-platform desktop notifications |
| `notebook_edit` | Edit Jupyter notebooks |

### Transport Adapters

`jiuwenClaw` provides two reference transport adapters as standalone static libraries:

| Adapter | CMake target | What it does |
|---------|--------------|--------------|
| HTTP server | `jiuwenClaw_http_server_adapter` | HTTP REST API with SSE streaming + built-in Web UI |
| Feishu bot | `jiuwenClaw_feishu_adapter` | Native Feishu (Lark) bot integration over WebSocket |

The core library does not depend on either of these — you can reuse them, swap them out, or write your own
adapters against `SessionManager`.

## Directory Structure

```
examples/jiuwenClaw/
├── main.cpp                          # Application entry point (SessionManager driven)
├── heartbeat_manager.{h,cpp}         # Heartbeat module (uses __HEARTBEAT__ session)
├── cron_watcher.{h,cpp}              # Cron scheduler (uses __CRON__ session)
├── adapters/
│   ├── http_server/                  # HTTP REST + SSE adapter
│   │   ├── http_server.{h,cpp}
│   └── feishu/                       # Feishu WebSocket bot adapter
│       ├── feishu_bot.{h,cpp}
│       └── feishu_channel.{h,cpp}
├── channels/
│   └── channel_manager.{h,cpp}       # Persists channel definitions to channels.json
├── models/
│   └── ark_code_model.{h,cpp}        # Custom OpenAI-compatible provider example
├── tools/
│   ├── cron_tool.{h,cpp}             # Cron management tool
│   ├── notify_tool.{h,cpp}           # Desktop notification tool
│   └── notebook_edit_tool.{h,cpp}    # Jupyter notebook editing tool
├── utils/                            # Local helpers (encoding, logger, string utils, ...)
├── templates/                        # Prompt templates (see "Prompt Templates" below)
├── web/
│   └── index.html                    # Built-in Web UI (served when --server is enabled)
└── doc/
    ├── en/                           # English documentation
    └── cn/                           # Chinese documentation
```

## Build & Run

### Build

```bash
# Linux
./build_linux.sh

# Windows (from "x64 Native Tools Command Prompt for VS")
build_windows.bat
```

Build artifacts are placed under `dist/<platform>/`. See the [top-level README](../../README.md) for
environment requirements and vcpkg notes.

### Run

```bash
# Linux (default: interactive CLI)
LD_LIBRARY_PATH=./libs:./dist/linux ./dist/linux/jiuwenClaw

# Windows
$env:PATH = "$PWD\dist\windows;$env:PATH"
.\dist\windows\jiuwenClaw.exe
```

### Server Mode (HTTP API + Web UI + configured channels)

```bash
# Linux
./dist/linux/jiuwenClaw --server

# Custom host/port
./dist/linux/jiuwenClaw --server --host 0.0.0.0 --port 9000

# Daemon mode (no CLI; HTTP + channels only)
./dist/linux/jiuwenClaw --server --no-cli
```

Web UI:
```
http://localhost:8080
```

### Command Line Options

```
jiuwenClaw [OPTIONS]
  --server       Enable HTTP REST API + Web UI + all configured channels
  --port <N>     Set server port (default: 8080)
  --host <IP>    Set server host (default: 127.0.0.1)
  --no-cli       Disable the interactive CLI (run only as a daemon; combine with --server)
  --help         Show this help message
```

When `--server` is set, channels listed in `./data/channels.json` are loaded and any enabled channel is started.

## CLI Commands

Once the CLI is running, the following slash commands are available:

| Command | Description |
|---------|-------------|
| `/exit` | Quit the program |
| `/session <id>` | Switch to the given session (created automatically if missing) |
| `/sessions` | List active sessions and mark the current one |

## Channel Configuration

Channels are persisted to `./data/channels.json` and can be edited either:

- **Via the Web UI** under `/api/channels` (when running with `--server`)
- **By hand** in the JSON file

Each entry has a `type` (currently `feishu`), an `id`, a human-readable `name`, an `enabled` flag, and a
`params` map. Example for a Feishu bot:

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

The CLI and HTTP transports do **not** appear here — they are controlled by the command-line flags above.

## Heartbeat

The heartbeat manager periodically inspects `./data/HEARTBEAT.md`. Tasks declared under the
`## Active Tasks` section are evaluated and (when due) dispatched through the `__HEARTBEAT__` reserved session:

```markdown
## Active Tasks

- Summarize project updates once a day
- Monitor disk usage every hour
```

If the section is empty or only contains comments, the heartbeat cycle is skipped.

## Cron Tasks

`CronWatcher` is an independent scheduler that uses the `__CRON__` reserved session to invoke
the agent when a trigger fires. It does **not** depend on the heartbeat module.

| Type | Description | Parameter |
|------|-------------|-----------|
| `one-time` | Fires once, then is removed | `at` (ISO timestamp) |
| `recurring` | Fires repeatedly at a fixed interval | `every_seconds` |
| `cron` | Fires according to a cron expression | `cron_expr` (e.g. `"0 9 * * 1-5"`) |

Most users create cron entries via natural-language interaction with the agent:

```
"Remind me to drink water in 10 minutes"
"Notify me every weekday at 9am for the morning stand-up"
```

## Configuration (in `main.cpp`)

Edit these blocks in `examples/jiuwenClaw/main.cpp` for your environment:

1. **LLM endpoint**
   ```cpp
   config.modelConfig.baseUrl = "<your-llm-endpoint>/v1";
   config.modelConfig.apiKey = "<your-api-key>";
   config.modelConfig.modelName = "<your-model-name>";
   config.modelConfig.formatType = ModelFormatType::OPENAI;
   // config.modelConfig.provider = "ark_code"; // when using a custom provider
   ```

2. **Amap MCP key** (Streamable HTTP MCP example)
   ```cpp
   "endpoint": "/mcp?key=<your-amap-key>"
   ```

3. **Skill directory**
   ```cpp
   config.skillDirectory = "./my_skills";
   ```

## Prompt Templates

`jiuwenClaw` uses a modular prompt template system. Each file has a clear responsibility and is referenced by
its placeholder name in `REACT_SYSTEM.md`:

| File | Responsibility | Placeholder |
|------|---------------|-------------|
| `AGENTS.md` | Agent capability list and behavior rules | `{$agents}` |
| `SOUL.md` | Agent personality and communication style | `{$soul}` |
| `USER.md` | User profile and preferences | `{$user}` |
| `TOOLS.md` | Tool usage policies (safety rules, search strategy) | `{$tools_md}` |
| `REACT_SYSTEM.md` | Master template — assembles all components + runtime context | (entry point) |
| `MEMORY.md` | Template used by Dream memory consolidation | — |

## Custom Model Provider

`jiuwenClaw` demonstrates the provider mechanism with `ArkCodeModel`, an OpenAI-compatible backend that
requires custom handling for `role=tool` messages:

```cpp
// 1. Register the provider with a unique name
auto& rm = ResourceManager::GetInstance();
rm.RegisterModel("ark_code", [](const ModelConfig& cfg) {
    return std::make_unique<ArkCodeModel>(cfg);
});

// 2. Select it via ModelConfig::provider
config.modelConfig.provider = "ark_code";
config.modelConfig.baseUrl = "<your-endpoint>/v3";
config.modelConfig.apiKey = "<your-api-key>";
config.modelConfig.modelName = "<your-model-name>";
```

When `provider` is non-empty, `ResourceManager::CreateModel` dispatches to the registered factory instead of
falling back to the built-in OpenAI/Anthropic implementations selected by `ModelConfig::formatType`.

## Building Your Own Agent on jiuwen-lite

To create a new agent application based on jiuwen-lite:

1. Create a new directory under `examples/` (or in your own repository)
2. Configure `promptTemplates` to point at your template files
3. Register any tools you need (built-in, MCP, or custom)
4. (Optional) Register custom model providers via `ResourceManager::RegisterModel`
5. Call `InitSessionManager(config)` to bring up the runtime
6. Drive interactions through `GetSessionManager().Invoke(sessionId, query, callback)`

The core jiuwen-lite library contains no business logic — all customization (prompts, tools, skills,
transports, channels) belongs in your application.

## Documentation

- **English**: this file
- **中文**: 请查看 [`doc/cn/README.md`](doc/cn/README.md)

## Quick Links

- [Framework homepage](../../README.md)
- [Release notes](../../release_notes/)

## License

Apache 2.0

# Memory Runtime

Memory Runtime is a standalone memory layer for agent runtimes. It provides short-term payload offload, long-term consolidation, structured entity/relation memory, REST APIs, and an MCP server.

## Deliverables

```text
bin/memory-server
bin/memory-mcp-server
lib/libagent_framework.so
include/
examples/memory_server/
```

## Build

From the jiuwen-lite repository:

```bash
./build_memory_runtime_linux.sh
```

Output:

```text
dist/memory-runtime/linux
```

## SDK Mode

Use SDK mode when the host is a C++ runtime and can link the memory library directly.

```cpp
MemoryConfig config;
config.enabled = true;
config.mode = "sdk";
config.dataPath = "./data";
config.enablePayloadOffload = true;

BuiltinMemoryRuntime runtime(config);
runtime.AppendEvent(event);
auto context = runtime.BuildContext(request);
runtime.Consolidate(consolidationRequest);
```

Main APIs:

```text
AppendEvent(event)
WritePayload(request)
ReadPayload(ref)
BuildContext(request)
Consolidate(request)
SearchMemory(request)
GetStats()
```

## HTTP Mode

Use HTTP mode when the host is written in another language or should talk to Memory Runtime as a sidecar.

Start server:

```bash
./bin/memory-server --host 127.0.0.1 --port 8090 --data ./data
```

Optional LLM consolidation:

```bash
cp examples/memory_server/model_config.example.json model_config.local.json
./bin/memory-server --host 127.0.0.1 --port 8090 --data ./data --model-config ./model_config.local.json
```

Endpoints:

```text
POST /v1/events
POST /v1/context
POST /v1/payloads
GET  /v1/payloads/{ref}
POST /v1/consolidate
POST /v1/search
GET  /v1/stats
GET  /health
```

Example:

```bash
curl -X POST http://127.0.0.1:8090/v1/events \
  -H 'Content-Type: application/json' \
  -d '{"type":2,"agentId":"agent-1","sessionId":"session-1","role":"user","content":"I prefer concise answers"}'

curl -X POST http://127.0.0.1:8090/v1/consolidate \
  -H 'Content-Type: application/json' \
  -d '{"agentId":"agent-1","sessionId":"session-1","force":true}'

curl -X POST http://127.0.0.1:8090/v1/context \
  -H 'Content-Type: application/json' \
  -d '{"agentId":"agent-1","sessionId":"session-1","query":"answer the user"}'
```

## MCP Mode

Use MCP mode when the host supports Model Context Protocol tools.

Start server:

```bash
./bin/memory-mcp-server --data ./data
```

Tools:

```text
memory_append_event
memory_build_context
memory_read_payload
memory_consolidate
memory_search
memory_stats
```

Optional LLM consolidation:

```bash
./bin/memory-mcp-server --data ./data --model-config ./model_config.local.json
```

## jiuwen-lite Server Mode

In `agents.json`:

```json
{
  "memoryConfig": {
    "enabled": true,
    "mode": "server",
    "serverUrl": "http://127.0.0.1:8090",
    "serverApiKey": "<optional memory server api key>",
    "serverTimeoutSeconds": 10,
    "enablePayloadOffload": true,
    "enableHierarchicalSummary": true,
    "enableEntityGraph": true
  }
}
```

See `examples/memory_server/agents_server_mode.example.json`.

## Minimum Dependency Boundary

Core runtime:

- C++17
- SQLite3
- nlohmann/json

Optional components:

- libcurl for `HttpMemoryRuntime`
- cpp-httplib for `memory-server`
- host-provided model client for LLM consolidation

The runtime must remain usable without an LLM model; consolidation falls back to rule-based extraction.

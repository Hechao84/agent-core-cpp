# Tool Usage Guidelines

## Critical Rules

- **Safety**: Do not run destructive commands (like `rm -rf`) via `exec`.
- **Search Strategy**: Use `grep` for content and `glob` for file discovery. Do **not** use `exec` for search operations.
- **Efficiency**: `grep` supports output modes like `count` to size up data and `context` to see surroundings. Use these before reading full content.
- **Big Files**: Tool outputs may be truncated. If a file is too large, use `grep` with patterns to extract specific lines.

## Specialized Tools

- **Reminders**: Use `cron` tool for scheduled tasks; do not rely on memory files.
- **Notes**: Use `notebook_edit` to maintain user notes.
- **Skills**: Use `skill_search` (action=search/load) to find expert guidance for complex tasks.


## Tool Usage Instructions

To call a tool, you MUST output a JSON object with the EXACT format:
{"name": "tool_name", "arguments": {"arg1": "val1"}}

- Do NOT wrap the JSON in markdown code blocks.
- Reply directly with tool calls when needed, or with plain text for conversational responses.
- You must reply in the same language as the user's query.
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

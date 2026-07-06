#pragma once

#include <mutex>
#include <string>
#include <vector>
#include "include/memory_types.h"
#include "include/types.h"

namespace jiuwen {

// HistoryStore is the local simplified MemoryRuntime fallback: when the
// primary MemoryRuntime is not configured (or its init failed), the
// ContextEngine event sink routes here instead. It stores the full event
// stream (aligned with MemoryRuntime's MemoryEvent fields) to a local JSONL
// file, which DreamProcessor mines for consolidation. Owned by
// SessionManager (survives ReloadAgent, like memoryRuntime_).
class HistoryStore
{
public:
    explicit HistoryStore(const std::string& basePath);
    ~HistoryStore() = default;

    // Append a full memory event (aligned with MemoryRuntime::AppendEvent).
    // Stores role/content/toolCallId/toolName/payloadRef/timestamp + cursor
    // + sessionId to the JSONL. Used as the ContextEngine sink target when
    // no MemoryRuntime is configured.
    void AppendEvent(const MemoryEvent& event);
    std::vector<HistoryEntry> ReadUnprocessedHistory(int sinceCursor);
    int GetLastDreamCursor() const;
    void SetLastDreamCursor(int cursor);
    void CompactHistory(int maxEntries);

private:
    std::string basePath_;
    std::string historyFile_;
    std::string cursorFile_;
    std::string dreamCursorFile_;
    mutable std::mutex mutex_;

    int NextCursor();
    int ReadCursor();
    int ReadDreamCursor();
    std::vector<HistoryEntry> ReadAllEntries();
    void WriteEntries(const std::vector<HistoryEntry>& entries);
};

} // namespace jiuwen

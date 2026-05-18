#pragma once

#include <mutex>
#include <string>
#include <vector>
#include "include/types.h"

namespace jiuwen {

class HistoryStore
{
public:
    explicit HistoryStore(const std::string& basePath);
    ~HistoryStore() = default;

    int AppendEntry(const std::string& role, const std::string& content);
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

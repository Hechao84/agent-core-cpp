#include "src/core/history_store.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include "third_party/include/nlohmann/json.hpp"

namespace fs = std::filesystem;

namespace jiuwen {

HistoryStore::HistoryStore(const std::string& basePath)
    : basePath_(basePath)
{
    fs::path memDir = fs::path(basePath_) / "memory";
    fs::create_directories(memDir);

    historyFile_ = (memDir / "history.jsonl").string();
    cursorFile_ = (memDir / ".cursor").string();
    dreamCursorFile_ = (memDir / ".dream_cursor").string();
}

int HistoryStore::ReadCursor()
{
    std::ifstream file(cursorFile_);
    if (file.is_open()) {
        std::string line;
        if (std::getline(file, line)) {
            try {
                return std::stoi(line);
            } catch (...) {
                return 0;
            }
        }
    }
    return 0;
}

int HistoryStore::ReadDreamCursor()
{
    std::ifstream file(dreamCursorFile_);
    if (file.is_open()) {
        std::string line;
        if (std::getline(file, line)) {
            try {
                return std::stoi(line);
            } catch (...) {
                return 0;
            }
        }
    }
    return 0;
}

int HistoryStore::NextCursor()
{
    int current = ReadCursor();
    return current + 1;
}

int HistoryStore::AppendEntry(const std::string& role, const std::string& content)
{
    std::lock_guard<std::mutex> lock(mutex_);

    int cursor = NextCursor();

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ts;
    ts << std::put_time(std::gmtime(&time), "%Y-%m-%d %H:%M");

    nlohmann::json entry;
    entry["cursor"] = cursor;
    entry["timestamp"] = ts.str();
    entry["role"] = role;
    entry["content"] = content;

    std::ofstream file(historyFile_, std::ios::app);
    if (file.is_open()) {
        file << entry.dump() << "\n";
    }

    std::ofstream cursorFile(cursorFile_, std::ios::trunc);
    if (cursorFile.is_open()) {
        cursorFile << cursor;
    }

    return cursor;
}

std::vector<HistoryEntry> HistoryStore::ReadAllEntries()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<HistoryEntry> entries;
    std::ifstream file(historyFile_);
    if (!file.is_open()) return entries;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        try {
            nlohmann::json j = nlohmann::json::parse(line);
            HistoryEntry e;
            e.cursor = j.value("cursor", 0);
            e.timestamp = j.value("timestamp", "");
            e.role = j.value("role", "");
            e.content = j.value("content", "");
            entries.push_back(e);
        } catch (...) {
            // Skip malformed lines
            continue;
        }
    }
    return entries;
}

std::vector<HistoryEntry> HistoryStore::ReadUnprocessedHistory(int sinceCursor)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<HistoryEntry> entries;
    std::ifstream file(historyFile_);
    if (!file.is_open()) return entries;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        try {
            nlohmann::json j = nlohmann::json::parse(line);
            int entryCursor = j.value("cursor", 0);
            if (entryCursor <= sinceCursor) continue;

            HistoryEntry e;
            e.cursor = entryCursor;
            e.timestamp = j.value("timestamp", "");
            e.role = j.value("role", "");
            e.content = j.value("content", "");
            entries.push_back(e);
        } catch (...) {
            continue;
        }
    }
    return entries;
}

int HistoryStore::GetLastDreamCursor() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return const_cast<HistoryStore*>(this)->ReadDreamCursor();
}

void HistoryStore::SetLastDreamCursor(int cursor)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream file(dreamCursorFile_, std::ios::trunc);
    if (file.is_open()) {
        file << cursor;
    }
}

void HistoryStore::CompactHistory(int maxEntries)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<HistoryEntry> entries = ReadAllEntries();
    if (static_cast<int>(entries.size()) <= maxEntries) return;

    auto start = entries.end() - maxEntries;
    std::vector<HistoryEntry> kept(start, entries.end());
    WriteEntries(kept);
}

void HistoryStore::WriteEntries(const std::vector<HistoryEntry>& entries)
{
    std::string tmpPath = historyFile_ + ".tmp";
    std::ofstream file(tmpPath);
    if (!file.is_open()) return;

    for (const auto& e : entries) {
        nlohmann::json j;
        j["cursor"] = e.cursor;
        j["timestamp"] = e.timestamp;
        j["role"] = e.role;
        j["content"] = e.content;
        file << j.dump() << "\n";
    }
    file.flush();

    fs::path tmp(tmpPath);
    fs::path orig(historyFile_);
    fs::rename(tmp, orig);
}

} // namespace jiuwen

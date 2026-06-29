#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "include/agent_export.h"

namespace jiuwen {

// Polls a set of files for mtime changes and fires a callback when any
// of them is modified. Modeled after SkillEngine's mtime check, but
// runs on its own thread (like CronWatcher).
class AGENT_API ConfigWatcher
{
public:
    using Callback = std::function<void(const std::string& changedPath)>;

    void Watch(const std::string& path, Callback cb);

    void Start(int pollSeconds = 3);
    void Stop();

    // Wake the watcher immediately (e.g. after an explicit /reload).
    void Poke();

    bool IsRunning() const { return running_.load(); }

private:
    struct Entry {
        std::string path;
        Callback cb;
        // Parentheses around 'min' defeat the Windows <windows.h> min/max
        // macros (which otherwise expand 'min()' into a macro call).
        std::filesystem::file_time_type lastMtime{
            (std::filesystem::file_time_type::min)()};
    };

    void Loop();

    std::mutex mutex_;  // Lock layer L5 (watch entries, not on Invoke thread)
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    int pollSeconds_{3};
    std::thread thread_;
    std::vector<Entry> entries_;
};

} // namespace jiuwen

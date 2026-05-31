#include "include/config/config_watcher.h"

#include <system_error>

#include "src/utils/logger.h"

namespace fs = std::filesystem;

namespace jiuwen {

void ConfigWatcher::Watch(const std::string& path, Callback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Entry e;
    e.path = path;
    e.cb = std::move(cb);
    std::error_code ec;
    if (fs::exists(path, ec) && !ec) {
        e.lastMtime = fs::last_write_time(path, ec);
    }
    entries_.push_back(std::move(e));
}

void ConfigWatcher::Start(int pollSeconds)
{
    if (running_.exchange(true)) return;
    pollSeconds_ = pollSeconds > 0 ? pollSeconds : 3;
    thread_ = std::thread([this]() { Loop(); });
    LOG(INFO) << "[ConfigWatcher] Started (poll=" << pollSeconds_ << "s, watching "
              << entries_.size() << " files)";
}

void ConfigWatcher::Stop()
{
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    LOG(INFO) << "[ConfigWatcher] Stopped";
}

void ConfigWatcher::Poke()
{
    cv_.notify_all();
}

void ConfigWatcher::Loop()
{
    while (running_.load()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& e : entries_) {
                std::error_code ec;
                if (!fs::exists(e.path, ec) || ec) continue;
                auto mt = fs::last_write_time(e.path, ec);
                if (ec) continue;
                if (e.lastMtime == fs::file_time_type::min()) {
                    // Initialise on first sighting; do not fire callback.
                    e.lastMtime = mt;
                    continue;
                }
                if (mt != e.lastMtime) {
                    e.lastMtime = mt;
                    LOG(INFO) << "[ConfigWatcher] Change detected: " << e.path;
                    try {
                        if (e.cb) e.cb(e.path);
                    } catch (const std::exception& ex) {
                        LOG(ERR) << "[ConfigWatcher] Callback threw: " << ex.what();
                    }
                }
            }
        }
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(pollSeconds_),
                     [this](){ return !running_.load(); });
    }
}

} // namespace jiuwen

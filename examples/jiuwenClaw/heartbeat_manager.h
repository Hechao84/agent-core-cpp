#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "include/session_manager.h"
#include "include/types.h"

namespace jiuwenClaw {

class HeartbeatManager {
public:
    HeartbeatManager(
        const std::string& heartbeatFilePath,
        const jiuwen::ModelConfig& modelConfig,
        int intervalSeconds = 1800
    );
    ~HeartbeatManager();

private:
    std::string ReadFile() const;
    bool DecideAction(const std::string& content, std::string& tasksSummary);
    void Run();

    std::string path_;
    jiuwen::ModelConfig modelConfig_;
    int intervalSeconds_{600};
    std::atomic<bool> running_{true};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace jiuwenClaw

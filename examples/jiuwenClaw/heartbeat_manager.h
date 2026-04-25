#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "include/agent.h"
#include "include/types.h"

using namespace jiuwen;

namespace jiuwenClaw {

class HeartbeatManager {
public:
    HeartbeatManager(
        const std::string& heartbeatFilePath,
        Agent& agent,
        const ModelConfig& modelConfig,
        int intervalSeconds = 1800
    );
    ~HeartbeatManager();

private:
    std::string ReadFile() const;
    bool DecideAction(const std::string& content, std::string& tasksSummary);
    void Run();

    std::string path_;
    Agent& agent_;
    ModelConfig modelConfig_;
    int intervalSeconds_{600};
    std::atomic<bool> running_{true};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace jiuwenClaw

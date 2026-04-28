#pragma once

#include <atomic>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "include/agent.h"
#include "include/types.h"

using namespace jiuwen;

namespace jiuwenClaw {

time_t CalculateNextFire(const std::string& cronExpr, time_t afterTime);

struct CronTriggerEvent {
    std::string message;
    std::string jobId;
    std::string jobType;
    bool isDeletion;
};

class CronWatcher {
public:
    CronWatcher(
        const std::string& dataDir,
        Agent& agent,
        const ModelConfig& modelConfig,
        int intervalSeconds = 60
    );
    ~CronWatcher();

    void Notify();

private:
    void Run();
    void CheckAndFireCrons();
    void HandleEvent(const CronTriggerEvent& event);

    time_t CalculateSleepDuration();

    std::string dataDir_;
    Agent& agent_;
    ModelConfig modelConfig_;
    int intervalSeconds_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace jiuwenClaw

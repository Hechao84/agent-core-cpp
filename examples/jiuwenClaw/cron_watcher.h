#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include "include/agent.h"
#include "include/types.h"

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

private:
    void Run();
    void CheckAndFireCrons();
    void HandleEvent(const CronTriggerEvent& event);

    std::string dataDir_;
    Agent& agent_;
    ModelConfig modelConfig_;
    int intervalSeconds_;
    std::atomic<bool> running_;
    std::thread thread_;
};

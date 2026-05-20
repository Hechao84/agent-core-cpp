#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <atomic>

#include "include/agent_export.h"
#include "include/session_manager.h"

namespace jiuwen {

struct WebApiConfig
{
    std::string host = "127.0.0.1";
    int port = 8080;
    bool enableCors = true;
    std::string staticDir; // Path to frontend static files (empty to disable)
};

class AGENT_API WebApi
{
public:
    WebApi();
    ~WebApi();

    void Start(const WebApiConfig& config);
    void Stop();

    bool IsRunning() const;
    std::string GetUrl() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::string url_;
};

} // namespace jiuwen

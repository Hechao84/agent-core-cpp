#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>

namespace jiuwenClaw {

struct HttpServerConfig
{
    std::string host = "127.0.0.1";
    int port = 8080;
    bool enableCors = true;
    std::string staticDir;
};

class HttpServer
{
public:
    HttpServer();
    ~HttpServer();

    void Start(const HttpServerConfig& config);
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

} // namespace jiuwenClaw

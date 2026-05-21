#define CPPHTTPLIB_KEEPALIVE_TIMEOUT_SECOND 10
#define CPPHTTPLIB_KEEPALIVE_MAX_COUNT 5
#include "httplib.h"
#include "src/web/web_api.h"
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <string>
#include <thread>
#include "src/context_engine/context_engine.h"
#include "src/utils/logger.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

struct WebApi::Impl
{
    std::unique_ptr<httplib::Server> server;
    std::thread serverThread;
    WebApiConfig config;
};

WebApi::WebApi() = default;

WebApi::~WebApi()
{
    Stop();
}

void WebApi::Start(const WebApiConfig& config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        LOG(WARN) << "[WebApi] Already running";
        return;
    }

    impl_ = std::make_unique<Impl>();
    impl_->config = config;
    impl_->server = std::make_unique<httplib::Server>();

    url_ = "http://" + config.host + ":" + std::to_string(config.port);

    if (config.enableCors) {
        impl_->server->set_pre_routing_handler(
            [](const httplib::Request& req, httplib::Response& res) {
                (void)req;
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
                res.set_header("Access-Control-Allow-Headers", "Content-Type, Accept");
                if (req.method == "OPTIONS") {
                    res.status = 204;
                    return httplib::Server::HandlerResponse::Handled;
                }
                return httplib::Server::HandlerResponse::Unhandled;
            }
        );
    }

    // GET /api/health
    impl_->server->Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        nlohmann::json result;
        result["status"] = "ok";
        result["sessions"] = GetSessionManager().GetSessionIds().size();
        res.set_content(result.dump(2), "application/json");
    });

    // GET /api/sessions
    impl_->server->Get("/api/sessions", [](const httplib::Request&, httplib::Response& res) {
        auto ids = GetSessionManager().GetSessionIds();
        nlohmann::json sessions = nlohmann::json::array();
        for (const auto& id : ids) {
            if (id == kHeartbeatSessionId || id == kCronSessionId) {
                continue;
            }
            nlohmann::json entry;
            entry["id"] = id;
            entry["busy"] = GetSessionManager().IsSessionBusy(id);
            entry["metadata"] = GetSessionManager().GetSessionMetadata(id);
            sessions.push_back(entry);
        }
        nlohmann::json result;
        result["sessions"] = sessions;
        result["count"] = sessions.size();
        res.set_content(result.dump(2), "application/json");
    });

    // POST /api/sessions
    impl_->server->Post("/api/sessions", [](const httplib::Request& req, httplib::Response& res) {
        std::string sessionId;
        try {
            auto j = nlohmann::json::parse(req.body);
            sessionId = j.value("id", "");
        } catch (...) {
            nlohmann::json err;
            err["error"] = "Invalid JSON";
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
            return;
        }

        if (sessionId.empty()) {
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count() % 10000;
            sessionId = "session_" + std::to_string(ms);
        }

        GetSessionManager().GetOrCreateSession(sessionId);

        nlohmann::json result;
        result["id"] = sessionId;
        result["created"] = true;
        res.status = 201;
        res.set_content(result.dump(2), "application/json");
    });

    // DELETE /api/sessions/{id}
    impl_->server->Delete(R"(/api/sessions/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string sessionId = req.matches[1];

        if (sessionId == kDefaultSessionId || sessionId == kHeartbeatSessionId || sessionId == kCronSessionId) {
            nlohmann::json err;
            err["error"] = "Cannot delete reserved session";
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
            return;
        }

        GetSessionManager().RemoveSession(sessionId);

        nlohmann::json result;
        result["id"] = sessionId;
        result["deleted"] = true;
        res.set_content(result.dump(2), "application/json");

    });

    // POST /api/sessions/{id}/cancel
    impl_->server->Post(R"(/api/sessions/([^/]+)/cancel)", [](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        std::string sessionId = req.matches[1];
        GetSessionManager().Cancel();
        nlohmann::json result;
        result["message"] = "cancel requested";
        result["session_id"] = sessionId;
        res.set_content(result.dump(2), "application/json");
    });

    // GET /api/sessions/{id}/history
    impl_->server->Get(R"(/api/sessions/([^/]+)/history)", [](const httplib::Request& req, httplib::Response& res) {
        std::string sessionId = req.matches[1];

        try {
            auto ctx = GetSessionManager().GetOrCreateSession(sessionId);
            if (!ctx) {
                nlohmann::json err;
                err["error"] = "Session not found";
                res.status = 404;
                res.set_content(err.dump(2), "application/json");
                return;
            }

            auto messages = ctx->GetAllMessages();
            nlohmann::json history = nlohmann::json::array();
            for (const auto& msg : messages) {
                nlohmann::json entry;
                entry["role"] = msg.role;
                entry["content"] = msg.content;
                history.push_back(entry);
            }

            nlohmann::json result;
            result["session_id"] = sessionId;
            result["messages"] = history;
            result["count"] = history.size();
            res.set_content(result.dump(2), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = "Failed to retrieve history";
            err["detail"] = e.what();
            res.status = 500;
            res.set_content(err.dump(2), "application/json");
        }
    });

    // POST /api/chat (non-streaming)
    impl_->server->Post("/api/chat", [](const httplib::Request& req, httplib::Response& res) {
        std::string sessionId;
        std::string message;
        try {
            auto j = nlohmann::json::parse(req.body);
            sessionId = j.value("session_id", kDefaultSessionId);
            message = j.value("message", "");
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = "Invalid JSON";
            err["detail"] = e.what();
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
            return;
        }

        if (message.empty()) {
            nlohmann::json err;
            err["error"] = "message is required";
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
            return;
        }

        SessionInvokeResult result = GetSessionManager().Invoke(
            sessionId,
            message,
            nullptr
        );

        nlohmann::json response;
        response["session_id"] = result.sessionId;
        response["success"] = result.success;
        response["content"] = result.content;
        if (!result.success) {
            response["error"] = result.errorMessage;
        }
        res.set_content(response.dump(2), "application/json");
    });

    // POST /api/chat/stream (SSE streaming)
    impl_->server->Post("/api/chat/stream", [](const httplib::Request& req, httplib::Response& res) {
        std::string sessionId;
        std::string message;
        try {
            auto j = nlohmann::json::parse(req.body);
            sessionId = j.value("session_id", kDefaultSessionId);
            message = j.value("message", "");
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = "Invalid JSON";
            err["detail"] = e.what();
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
            return;
        }

        if (message.empty()) {
            nlohmann::json err;
            err["error"] = "message is required";
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
            return;
        }

        LOG(INFO) << "[WebApi] [" << sessionId << "] Streaming chat requested";

        struct SseContext {
            std::vector<std::string> events;
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
            SessionInvokeResult result;
        };

        auto sseCtx = std::make_shared<SseContext>();

        auto streamCallback = [sseCtx](const std::string& resp) {
            if (resp.empty()) return;

            std::string eventType = "message";
            std::string data = resp;

            if (resp.find("[STREAM]") != std::string::npos) {
                eventType = "stream";
                size_t pos = resp.find("[STREAM]");
                data = resp.substr(pos + 8);
                while (!data.empty() && data[0] == ' ') data = data.substr(1);
            } else if (resp.find("[STATUS]") != std::string::npos) {
                eventType = "status";
                size_t pos = resp.find("[STATUS]");
                data = resp.substr(pos + 8);
                while (!data.empty() && data[0] == ' ') data = data.substr(1);
            } else if (resp.find("[TOOL_CALLS]") != std::string::npos) {
                eventType = "tool_call";
                size_t pos = resp.find("[TOOL_CALLS]");
                data = resp.substr(pos + 12);
                while (!data.empty() && (data[0] == ' ' || data[0] == '\n')) data = data.substr(1);
            } else if (resp.find("[TOOL_RESPONSE]") != std::string::npos) {
                eventType = "tool_response";
                size_t pos = resp.find("[TOOL_RESPONSE]");
                data = resp.substr(pos + 15);
                while (!data.empty() && data[0] == ' ') data = data.substr(1);
            } else if (resp.find("[FINAL]") != std::string::npos) {
                eventType = "done";
                data = "";
            }

            if (!data.empty()) {
                // Format as proper SSE with multiline data support
                std::string sseEvent = "event: " + eventType + "\n";
                size_t start = 0;
                size_t end = data.find('\n');
                while (end != std::string::npos) {
                    sseEvent += "data: " + data.substr(start, end - start) + "\n";
                    start = end + 1;
                    end = data.find('\n', start);
                }
                sseEvent += "data: " + data.substr(start) + "\n\n";

                std::lock_guard<std::mutex> lock(sseCtx->mutex);
                sseCtx->events.push_back(sseEvent);
                sseCtx->cv.notify_one();
            }
        };

        // Run the agent invoke in a background thread
        std::thread([sseCtx, sessionId, message, streamCallback]() {
            try {
                sseCtx->result = GetSessionManager().Invoke(
                    sessionId,
                    message,
                    streamCallback
                );
            } catch (...) {
                LOG(ERR) << "[WebApi] [" << sessionId << "] Agent invoke failed";
            }

            {
                std::lock_guard<std::mutex> lock(sseCtx->mutex);
                sseCtx->done = true;
            }
            sseCtx->cv.notify_one();
        }).detach();

        // Set up chunked response using SSE
        res.set_chunked_content_provider(
            "text/event-stream",
            [sseCtx](size_t /* offset */, httplib::DataSink& sink) -> bool {
                std::unique_lock<std::mutex> lock(sseCtx->mutex);

                sseCtx->cv.wait(lock, [&]() {
                    return !sseCtx->events.empty() || sseCtx->done;
                });

                while (!sseCtx->events.empty()) {
                    const auto& event = sseCtx->events.front();
                    if (!sink.write(event.data(), event.size())) {
                        return false;
                    }
                    sseCtx->events.erase(sseCtx->events.begin());
                }

                if (sseCtx->done) {
                    lock.unlock();

                    nlohmann::json finalResult;
                    finalResult["session_id"] = sseCtx->result.sessionId;
                    finalResult["success"] = sseCtx->result.success;
                    finalResult["content"] = sseCtx->result.content;
                    if (!sseCtx->result.success) {
                        finalResult["error"] = sseCtx->result.errorMessage;
                    }

                    std::string doneEvent = "event: done\ndata: " + finalResult.dump() + "\n\n";
                    if (!sink.write(doneEvent.data(), doneEvent.size())) {
                        return false;
                    }
                    return false;
                }

                return true;
            }
        );
    });

    // Serve static frontend files if configured
    if (!config.staticDir.empty()) {
        impl_->server->set_mount_point("/", config.staticDir);
        LOG(INFO) << "[WebApi] Serving static files from " << config.staticDir;
    }

    // Start server in background thread
    impl_->serverThread = std::thread([this, config]() {
        LOG(INFO) << "[WebApi] Starting server at " << config.host << ":" << config.port;
        if (!impl_->server->listen(config.host.c_str(), config.port)) {
            LOG(ERR) << "[WebApi] Failed to start server";
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    running_ = true;
    LOG(INFO) << "[WebApi] Server running at " << url_;
}

void WebApi::Stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;

    running_ = false;

    if (impl_ && impl_->server) {
        impl_->server->stop();
    }

    if (impl_ && impl_->serverThread.joinable()) {
        impl_->serverThread.join();
    }

    impl_.reset();
    LOG(INFO) << "[WebApi] Stopped";
}

bool WebApi::IsRunning() const
{
    return running_;
}

std::string WebApi::GetUrl() const
{
    return url_;
}

} // namespace jiuwen

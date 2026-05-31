#define CPPHTTPLIB_KEEPALIVE_TIMEOUT_SECOND 10
#define CPPHTTPLIB_KEEPALIVE_MAX_COUNT 5
#include "httplib.h"

#include "examples/jiuwenClaw/adapters/http_server/http_server.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "examples/jiuwenClaw/channels/channel_manager.h"
#include "examples/jiuwenClaw/channels/channel_service.h"
#include "examples/jiuwenClaw/utils/logger.h"
#include "include/agent.h"
#include "include/config/agent_config_json.h"
#include "include/config/agent_config_store.h"
#include "include/resource_manager.h"
#include "include/session_manager.h"

#include "third_party/include/nlohmann/json.hpp"

namespace jiuwenClaw {

using namespace jiuwen;

struct HttpServer::Impl
{
    std::unique_ptr<httplib::Server> server;
    std::thread serverThread;
    HttpServerConfig config;
};

HttpServer::HttpServer() = default;

HttpServer::~HttpServer()
{
    Stop();
}

void HttpServer::Start(const HttpServerConfig& config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        LOG(WARN) << "[HttpServer] Already running";
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
            auto messages = GetSessionManager().GetSessionMessages(sessionId);
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

        LOG(INFO) << "[HttpServer] [" << sessionId << "] Streaming chat requested";

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

        std::thread([sseCtx, sessionId, message, streamCallback]() {
            try {
                sseCtx->result = GetSessionManager().Invoke(
                    sessionId,
                    message,
                    streamCallback
                );
            } catch (...) {
                LOG(ERR) << "[HttpServer] [" << sessionId << "] Agent invoke failed";
            }

            {
                std::lock_guard<std::mutex> lock(sseCtx->mutex);
                sseCtx->done = true;
            }
            sseCtx->cv.notify_one();
        }).detach();

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

    // GET /api/channels
    impl_->server->Get("/api/channels", [](const httplib::Request&, httplib::Response& res) {
        auto channels = ChannelManager::GetInstance().GetAllChannels();
        nlohmann::json result = nlohmann::json::array();
        for (const auto& ch : channels) {
            nlohmann::json entry;
            entry["id"] = ch.id;
            entry["type"] = ch.type;
            entry["name"] = ch.name;
            entry["enabled"] = ch.enabled;
            entry["params"] = ch.params;
            result.push_back(entry);
        }
        res.set_content(result.dump(2), "application/json");
    });

    // POST /api/channels - create channel
    impl_->server->Post("/api/channels", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = nlohmann::json::parse(req.body);
            ChannelConfig ch;
            ch.id = j.value("id", "");
            ch.type = j.value("type", "");
            ch.name = j.value("name", "");
            ch.enabled = j.value("enabled", true);

            if (j.contains("params")) {
                for (auto& [key, val] : j["params"].items()) {
                    ch.params[key] = val.get<std::string>();
                }
            }

            ChannelManager::GetInstance().AddChannel(ch);
            // Resolve auto-generated id (if any) and apply.
            auto created = ChannelManager::GetInstance().GetAllChannels();
            for (const auto& c : created) {
                if (c.type == ch.type && c.name == ch.name) {
                    ChannelService::Instance().Apply(c);
                    ch.id = c.id;
                    break;
                }
            }
            LOG(INFO) << "[HttpServer] Channel created: " << ch.id << " (" << ch.type << ")";

            nlohmann::json result;
            result["id"] = ch.id;
            result["created"] = true;
            res.status = 201;
            res.set_content(result.dump(2), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = "Invalid JSON";
            err["detail"] = e.what();
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
        }
    });

    // DELETE /api/channels/{id}
    impl_->server->Delete(R"(/api/channels/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string channelId = req.matches[1];
        ChannelManager::GetInstance().RemoveChannel(channelId);
        ChannelService::Instance().Remove(channelId);
        LOG(INFO) << "[HttpServer] Channel deleted: " << channelId;

        nlohmann::json result;
        result["id"] = channelId;
        result["deleted"] = true;
        res.set_content(result.dump(2), "application/json");
    });

    // PUT /api/channels/{id}
    impl_->server->Put(R"(/api/channels/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string channelId = req.matches[1];
        try {
            auto j = nlohmann::json::parse(req.body);
            ChannelConfig ch;
            ch.id = channelId;
            ch.type = j.value("type", "");
            ch.name = j.value("name", "");
            ch.enabled = j.value("enabled", true);

            if (j.contains("params")) {
                for (auto& [key, val] : j["params"].items()) {
                    ch.params[key] = val.get<std::string>();
                }
            }

            ChannelManager::GetInstance().UpdateChannel(channelId, ch);
            ChannelService::Instance().Apply(ch);
            LOG(INFO) << "[HttpServer] Channel updated: " << channelId;

            nlohmann::json result;
            result["id"] = channelId;
            result["updated"] = true;
            res.set_content(result.dump(2), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = "Invalid JSON";
            err["detail"] = e.what();
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
        }
    });

    // POST /api/channels/reload
    impl_->server->Post("/api/channels/reload", [](const httplib::Request&, httplib::Response& res) {
        ChannelManager::GetInstance().Load();
        ChannelService::Instance().ReconcileAll();
        nlohmann::json result;
        result["reloaded"] = true;
        result["active"] = ChannelService::Instance().ActiveIds();
        res.set_content(result.dump(2), "application/json");
    });

    // GET /api/tools - list available tools (for the agent form dropdown).
    impl_->server->Get("/api/tools", [](const httplib::Request&, httplib::Response& res) {
        auto tools = ResourceManager::GetInstance().GetAvailableTools();
        nlohmann::json result;
        result["tools"] = tools;
        result["count"] = tools.size();
        res.set_content(result.dump(2), "application/json");
    });

    // GET /api/skills - list installed skills (metadata only, no body)
    impl_->server->Get("/api/skills", [](const httplib::Request&, httplib::Response& res) {
        nlohmann::json result;
        nlohmann::json arr = nlohmann::json::array();
        std::string rootDir;
        auto agent = GetSessionManager().GetAgent();
        if (agent) {
            rootDir = agent->GetSkillRootDir();
            for (const auto& s : agent->ListSkills()) {
                nlohmann::json e;
                e["id"]          = s.id;
                e["name"]        = s.name.empty() ? s.id : s.name;
                e["description"] = s.description;
                e["directory"]   = s.directory;
                arr.push_back(e);
            }
        }
        result["skills"]   = arr;
        result["count"]    = arr.size();
        result["root_dir"] = rootDir;
        res.set_content(result.dump(2), "application/json");
    });

    // GET /api/skills/{id} - skill detail incl. body
    impl_->server->Get(R"(/api/skills/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        auto agent = GetSessionManager().GetAgent();
        if (!agent) {
            nlohmann::json err;
            err["error"] = "agent unavailable";
            res.status = 404;
            res.set_content(err.dump(2), "application/json");
            return;
        }
        auto skill = agent->GetSkill(id);
        if (skill.id.empty()) {
            nlohmann::json err;
            err["error"] = "skill not found";
            err["id"]    = id;
            res.status = 404;
            res.set_content(err.dump(2), "application/json");
            return;
        }
        nlohmann::json e;
        e["id"]          = skill.id;
        e["name"]        = skill.name.empty() ? skill.id : skill.name;
        e["description"] = skill.description;
        e["directory"]   = skill.directory;
        e["body"]        = skill.body;
        res.set_content(e.dump(2), "application/json");
    });

    // GET /api/agents - list all agents (merged: default + override)
    impl_->server->Get("/api/agents", [](const httplib::Request&, httplib::Response& res) {
        auto list = AgentConfigStore::Instance().List();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& cfg : list) {
            arr.push_back(AgentConfigToJson(cfg));
        }
        nlohmann::json result;
        result["agents"] = arr;
        result["count"] = arr.size();
        res.set_content(result.dump(2), "application/json");
    });

    // GET /api/agents/{id}
    impl_->server->Get(R"(/api/agents/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        auto cfgOpt = AgentConfigStore::Instance().Get(id);
        if (!cfgOpt) {
            nlohmann::json err;
            err["error"] = "agent not found";
            err["id"] = id;
            res.status = 404;
            res.set_content(err.dump(2), "application/json");
            return;
        }
        res.set_content(AgentConfigToJson(*cfgOpt).dump(2), "application/json");
    });

    // PUT /api/agents/{id} - update + hot-reload
    impl_->server->Put(R"(/api/agents/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        try {
            auto j = nlohmann::json::parse(req.body);
            // Build the effective config: start from current effective then merge.
            AgentConfig base;
            base.id = id;
            auto cur = AgentConfigStore::Instance().Get(id);
            if (cur) base = *cur;
            MergeAgentConfigFromJson(j, base);
            base.id = id;

            AgentConfigStore::Instance().Upsert(base);

            // Hot-reload only if this id matches the live agent.
            std::string liveId = GetSessionManager().GetConfig().id;
            if (liveId == id || liveId.empty()) {
                std::string reloadErr;
                if (!GetSessionManager().ReloadAgent(base, &reloadErr)) {
                    nlohmann::json err;
                    err["error"] = "agent rebuild failed";
                    err["detail"] = reloadErr;
                    res.status = 500;
                    res.set_content(err.dump(2), "application/json");
                    return;
                }
            }

            nlohmann::json result;
            result["id"] = id;
            result["updated"] = true;
            result["reloaded"] = (liveId == id || liveId.empty());
            res.set_content(result.dump(2), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = "Invalid JSON";
            err["detail"] = e.what();
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
        }
    });

    // DELETE /api/agents/{id} - drop override (revert to code default)
    impl_->server->Delete(R"(/api/agents/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        AgentConfigStore::Instance().Remove(id);
        // If this is the live agent, reload to fall back on the default.
        std::string liveId = GetSessionManager().GetConfig().id;
        if (liveId == id) {
            auto cfgOpt = AgentConfigStore::Instance().Get(id);
            if (cfgOpt) {
                std::string reloadErr;
                if (!GetSessionManager().ReloadAgent(*cfgOpt, &reloadErr)) {
                    LOG(WARN) << "[HttpServer] Reload after delete failed: " << reloadErr;
                }
            }
        }
        nlohmann::json result;
        result["id"] = id;
        result["override_removed"] = true;
        res.set_content(result.dump(2), "application/json");
    });

    // POST /api/agents/reload - re-read agents.json and hot-reload the live agent
    impl_->server->Post("/api/agents/reload", [](const httplib::Request&, httplib::Response& res) {
        auto eff = AgentConfigStore::Instance().Load();
        std::string liveId = GetSessionManager().GetConfig().id;
        nlohmann::json result;
        result["count"] = eff.size();
        auto it = eff.find(liveId);
        if (it != eff.end()) {
            std::string reloadErr;
            if (GetSessionManager().ReloadAgent(it->second, &reloadErr)) {
                result["reloaded_id"] = liveId;
                result["success"] = true;
            } else {
                result["success"] = false;
                result["error"] = reloadErr;
                res.status = 500;
            }
        } else {
            result["success"] = true;
            result["reloaded_id"] = nullptr;
            result["note"] = "no entry for live agent in agents.json; left untouched";
        }
        res.set_content(result.dump(2), "application/json");
    });

    // Serve static frontend files if configured
    if (!config.staticDir.empty()) {
        impl_->server->set_mount_point("/", config.staticDir);
        LOG(INFO) << "[HttpServer] Serving static files from " << config.staticDir;
    }

    // Start server in background thread
    impl_->serverThread = std::thread([this, config]() {
        LOG(INFO) << "[HttpServer] Starting server at " << config.host << ":" << config.port;
        if (!impl_->server->listen(config.host.c_str(), config.port)) {
            LOG(ERR) << "[HttpServer] Failed to start server";
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    running_ = true;
    LOG(INFO) << "[HttpServer] Server running at " << url_;
}

void HttpServer::Stop()
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
    LOG(INFO) << "[HttpServer] Stopped";
}

bool HttpServer::IsRunning() const
{
    return running_;
}

std::string HttpServer::GetUrl() const
{
    return url_;
}

} // namespace jiuwenClaw

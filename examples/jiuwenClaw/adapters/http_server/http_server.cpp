#define CPPHTTPLIB_KEEPALIVE_TIMEOUT_SECOND 10
#define CPPHTTPLIB_KEEPALIVE_MAX_COUNT 5
#include "httplib.h"

#include "examples/jiuwenClaw/adapters/http_server/http_server.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "examples/jiuwenClaw/channels/channel_manager.h"
#include "examples/jiuwenClaw/channels/channel_service.h"
#include "examples/jiuwenClaw/mcp/mcp_server_manager.h"
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

    // GET /api/sessions/{id}/history -- structured messages with native
    // function-calling fields. Front-end consumes this to render tool
    // bubbles by id pairing (assistant.tool_calls[].id <-> tool.tool_call_id).
    impl_->server->Get(R"(/api/sessions/([^/]+)/history)", [](const httplib::Request& req, httplib::Response& res) {
        std::string sessionId = req.matches[1];

        try {
            auto messages = GetSessionManager().GetSessionMessages(sessionId);
            nlohmann::json history = nlohmann::json::array();
            for (const auto& msg : messages) {
                nlohmann::json entry;
                entry["role"] = msg.role;
                entry["content"] = msg.content;
                if (!msg.toolCalls.empty()) {
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto& tc : msg.toolCalls) {
                        nlohmann::json j;
                        j["id"] = tc.id;
                        j["name"] = tc.name;
                        j["arguments"] = tc.argumentsJson;
                        arr.push_back(j);
                    }
                    entry["tool_calls"] = arr;
                }
                if (!msg.toolCallId.empty()) {
                    entry["tool_call_id"] = msg.toolCallId;
                }
                if (!msg.toolName.empty()) {
                    entry["tool_name"] = msg.toolName;
                }
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
            if (resp.empty()) {
                return;
            }

            std::string eventType = "message";
            std::string data = resp;

            if (resp.find("[STREAM]") != std::string::npos) {
                eventType = "stream";
                size_t pos = resp.find("[STREAM]");
                data = resp.substr(pos + 8);
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
            } else if (resp.find("[TOOL_RESPONSE") != std::string::npos) {
                // Two on-wire forms supported:
                //   [TOOL_RESPONSE] <content>             (legacy)
                //   [TOOL_RESPONSE <call_id>] <content>   (native function-call)
                // In the second form the front-end uses call_id to route the
                // result back to the right tool bubble.
                eventType = "tool_response";
                size_t pos = resp.find("[TOOL_RESPONSE");
                size_t closeBracket = resp.find("]", pos);
                if (closeBracket == std::string::npos) {
                    data = resp.substr(pos + 14); // strlen("[TOOL_RESPONSE")
                } else {
                    std::string header = resp.substr(pos + 14, closeBracket - pos - 14);
                    // Strip a leading space (i.e. "[TOOL_RESPONSE call_xxx]" form).
                    while (!header.empty() && header.front() == ' ') header.erase(0, 1);
                    std::string payload = resp.substr(closeBracket + 1);
                    while (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
                    if (header.empty()) {
                        data = payload;
                    } else {
                        // Prefix payload with "<call_id>: " for the SSE so the
                        // front-end pattern-matches it.
                        data = header + ": " + payload;
                    }
                }
                while (!data.empty() && data[0] == '\n') data = data.substr(1);
            } else if (resp.find("[ASK_USER]") != std::string::npos) {
                eventType = "ask_user";
                size_t pos = resp.find("[ASK_USER]");
                size_t endPos = resp.find("[/ASK_USER]", pos);
                size_t startData = pos + 10; // strlen("[ASK_USER]")
                if (endPos != std::string::npos) {
                    data = resp.substr(startData, endPos - startData);
                } else {
                    data = resp.substr(startData);
                }
                while (!data.empty() && (data[0] == ' ' || data[0] == '\n')) data = data.substr(1);
            } else if (resp.find("[FINAL]") != std::string::npos) {
                eventType = "done";
                data = "";
            }

            size_t crpos;
            while ((crpos = data.find('\r')) != std::string::npos) {
                data.erase(crpos, 1);
            }

            if (!data.empty()) {
                std::string sseEvent = "event: " + eventType + "\n";
                if (eventType == "stream") {
                    sseEvent += "data: " + nlohmann::json(data).dump() + "\n\n";
                } else {
                    size_t start = 0;
                    size_t end = data.find('\n');
                    while (end != std::string::npos) {
                        sseEvent += "data: " + data.substr(start, end - start) + "\n";
                        start = end + 1;
                        end = data.find('\n', start);
                    }
                    sseEvent += "data: " + data.substr(start) + "\n\n";
                }

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
            if (cur) {
                base = *cur;
            }
            MergeAgentConfigFromJson(j, base);
            base.id = id;

            // Persist exactly what the Web UI submitted (editable fields only),
            // not the fully-merged effective config with all code defaults.
            AgentConfigStore::Instance().UpsertOverride(id, j);

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

    // --- MCP Server Management ---

    // GET /api/mcp_servers - list all (from persistent store + connected status)
    impl_->server->Get("/api/mcp_servers", [](const httplib::Request&, httplib::Response& res) {
        auto entries = McpServerManager::GetInstance().GetAllServers();
        auto active = ResourceManager::GetInstance().GetConnectedMCPServerIds();
        std::unordered_map<std::string, bool> isActive;
        for (const auto& activeId : active) {
            isActive[activeId] = true;
        }

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : entries) {
            nlohmann::json entry;
            entry["id"] = e.id;
            entry["name"] = e.name;
            entry["description"] = e.description;
            entry["enabled"] = e.enabled;
            entry["type"] = e.type;
            entry["url"] = e.url;
            entry["endpoint"] = e.endpoint;
            entry["command"] = e.command;
            entry["args"] = e.args;
            nlohmann::json env = nlohmann::json::object();
            for (const auto& kv : e.env) {
                env[kv.first] = kv.second;
            }
            entry["env"] = env;
            nlohmann::json headers = nlohmann::json::object();
            for (const auto& kv : e.headers) {
                headers[kv.first] = kv.second;
            }
            entry["headers"] = headers;
            entry["connected"] = (e.enabled && isActive.count(e.id) > 0);
            arr.push_back(entry);
        }
        res.set_content(arr.dump(2), "application/json");
    });

    // POST /api/mcp_servers - create
    impl_->server->Post("/api/mcp_servers", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = nlohmann::json::parse(req.body);
            McpServerEntry entry;
            entry.id = j.value("id", "");
            entry.name = j.value("name", "");
            entry.description = j.value("description", "");
            entry.enabled = j.value("enabled", true);
            entry.type = j.value("type", "");
            entry.url = j.value("url", "");
            entry.endpoint = j.value("endpoint", "");
            entry.command = j.value("command", "");
            if (j.contains("args") && j["args"].is_array()) {
                for (const auto& a : j["args"]) {
                    if (a.is_string()) {
                        entry.args.push_back(a.get<std::string>());
                    }
                }
            }
            if (j.contains("env") && j["env"].is_object()) {
                for (auto it = j["env"].begin(); it != j["env"].end(); ++it) {
                    if (it.value().is_string()) {
                        entry.env[it.key()] = it.value().get<std::string>();
                    }
                }
            }
            if (j.contains("headers") && j["headers"].is_object()) {
                for (auto it = j["headers"].begin(); it != j["headers"].end(); ++it) {
                    if (it.value().is_string()) {
                        entry.headers[it.key()] = it.value().get<std::string>();
                    }
                }
            }

            if (entry.id.empty()) {
                nlohmann::json err;
                err["error"] = "id required";
                res.status = 400;
                res.set_content(err.dump(2), "application/json");
                return;
            }

            // Persist to mcp_servers.json
            McpServerManager::GetInstance().AddServer(entry);

            ResourceManager::GetInstance().LoadMCPServers(McpServerManager::GetInstance().ToFrameworkConfigs());

            if (auto liveAgent = GetSessionManager().GetAgent()) {
                int delta = liveAgent->SyncMcpTools();
                LOG(INFO) << "[HttpServer] SyncMcpTools delta=" << delta;
            }

            nlohmann::json result;
            result["id"] = entry.id;
            result["created"] = true;
            res.status = 201;
            res.set_content(result.dump(2), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = e.what();
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
        }
    });

    // PUT /api/mcp_servers/{id} - update
    impl_->server->Put(R"(/api/mcp_servers/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string id = req.matches[1];
            auto j = nlohmann::json::parse(req.body);

            McpServerEntry entry;
            entry.id = id;
            entry.name = j.value("name", "");
            entry.description = j.value("description", "");
            entry.enabled = j.value("enabled", true);
            entry.type = j.value("type", "");
            entry.url = j.value("url", "");
            entry.endpoint = j.value("endpoint", "");
            entry.command = j.value("command", "");
            if (j.contains("args") && j["args"].is_array()) {
                for (const auto& a : j["args"]) {
                    if (a.is_string()) {
                        entry.args.push_back(a.get<std::string>());
                    }
                }
            }
            if (j.contains("env") && j["env"].is_object()) {
                for (auto it = j["env"].begin(); it != j["env"].end(); ++it) {
                    if (it.value().is_string()) {
                        entry.env[it.key()] = it.value().get<std::string>();
                    }
                }
            }
            if (j.contains("headers") && j["headers"].is_object()) {
                for (auto it = j["headers"].begin(); it != j["headers"].end(); ++it) {
                    if (it.value().is_string()) {
                        entry.headers[it.key()] = it.value().get<std::string>();
                    }
                }
            }

            // Persist update
            McpServerManager::GetInstance().UpdateServer(id, entry);

            ResourceManager::GetInstance().LoadMCPServers(McpServerManager::GetInstance().ToFrameworkConfigs());

            if (auto liveAgent = GetSessionManager().GetAgent()) {
                int delta = liveAgent->SyncMcpTools();
                LOG(INFO) << "[HttpServer] SyncMcpTools delta=" << delta;
            }

            nlohmann::json result;
            result["id"] = id;
            result["updated"] = true;
            res.set_content(result.dump(2), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = e.what();
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
        }
    });

    // DELETE /api/mcp_servers/{id} - delete
    impl_->server->Delete(R"(/api/mcp_servers/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];

        // Remove from persistent store
        McpServerManager::GetInstance().RemoveServer(id);

        // Disconnect from framework (this also unregisters the server's
        // tools from ResourceManager via MCPConnection::Disconnect)
        ResourceManager::GetInstance().UnregisterMCPServer(id);

        // Reconcile MCP tool list on the live Agent so the disconnected
        // server's tools are dropped from the prompt
        if (auto liveAgent = GetSessionManager().GetAgent()) {
            int delta = liveAgent->SyncMcpTools();
            LOG(INFO) << "[HttpServer] SyncMcpTools delta=" << delta;
        }

        // Cascade: remove this id from all agents' mcpServerIds. Persist only
        // Web-editable fields to avoid expanding agents.json with code defaults.
        auto allCfgs = AgentConfigStore::Instance().List();
        for (auto& agentCfg : allCfgs) {
            auto& ids = agentCfg.mcpServerIds;
            auto newEnd = std::remove(ids.begin(), ids.end(), id);
            if (newEnd != ids.end()) {
                ids.erase(newEnd, ids.end());
                nlohmann::json overrideJson;
                overrideJson["id"] = agentCfg.id;
                overrideJson["modelConfig"]["baseUrl"] = agentCfg.modelConfig.baseUrl;
                overrideJson["modelConfig"]["apiKey"] = agentCfg.modelConfig.apiKey;
                overrideJson["modelConfig"]["modelName"] = agentCfg.modelConfig.modelName;
                overrideJson["modelConfig"]["formatType"] = "openai";
                if (agentCfg.modelConfig.formatType == ModelFormatType::ANTHROPIC) {
                    overrideJson["modelConfig"]["formatType"] = "anthropic";
                }
                overrideJson["modelConfig"]["provider"] = agentCfg.modelConfig.provider;
                std::function<nlohmann::json(const ConfigValue&)> configValueToJson;
                configValueToJson = [&configValueToJson](const ConfigValue& v) -> nlohmann::json {
                    if (auto p = std::get_if<int>(&v)) return *p;
                    if (auto p = std::get_if<float>(&v)) return *p;
                    if (auto p = std::get_if<bool>(&v)) return *p;
                    if (auto p = std::get_if<std::string>(&v)) return *p;
                    if (auto p = std::get_if<std::vector<std::string>>(&v)) return *p;
                    if (auto p = std::get_if<std::shared_ptr<ConfigNode>>(&v)) {
                        nlohmann::json obj = nlohmann::json::object();
                        if (*p) {
                            for (const auto& child : (*p)->fields_) {
                                obj[child.first] = configValueToJson(child.second);
                            }
                        }
                        return obj;
                    }
                    return nullptr;
                };
                nlohmann::json ep = nlohmann::json::object();
                for (const auto& kv : agentCfg.modelConfig.extraParams.fields_) {
                    ep[kv.first] = configValueToJson(kv.second);
                }
                overrideJson["modelConfig"]["extraParams"] = ep;
                overrideJson["mcpServerIds"] = ids;
                AgentConfigStore::Instance().UpsertOverride(agentCfg.id, overrideJson);
            }
        }

        nlohmann::json result;
        result["id"] = id;
        result["deleted"] = true;
        res.set_content(result.dump(2), "application/json");
    });

    // POST /api/mcp_servers/reload - reload from mcp_servers.json
    impl_->server->Post("/api/mcp_servers/reload", [](const httplib::Request&, httplib::Response& res) {
        McpServerManager::GetInstance().Load();
        auto configs = McpServerManager::GetInstance().ToFrameworkConfigs();
        ResourceManager::GetInstance().LoadMCPServers(configs);
        if (auto liveAgent = GetSessionManager().GetAgent()) {
            int delta = liveAgent->SyncMcpTools();
            LOG(INFO) << "[HttpServer] SyncMcpTools delta=" << delta;
        }
        nlohmann::json result;
        result["reloaded"] = true;
        res.set_content(result.dump(2), "application/json");
    });

    // POST /api/answer - resolve a pending ask_user request emitted via the
    // [ASK_USER] SSE tag. Body: { "request_id": "...", "answer": "..." }.
    impl_->server->Post("/api/answer", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = nlohmann::json::parse(req.body);
            std::string requestId = j.value("request_id", std::string{});
            std::string answer = j.value("answer", std::string{});
            if (requestId.empty()) {
                nlohmann::json err;
                err["ok"] = false;
                err["error"] = "request_id required";
                res.status = 400;
                res.set_content(err.dump(2), "application/json");
                return;
            }
            bool ok = false;
            if (auto liveAgent = GetSessionManager().GetAgent()) {
                ok = liveAgent->ProvideUserResponse(requestId, answer);
            }
            nlohmann::json result;
            result["ok"] = ok;
            if (!ok) {
                result["error"] = "request_id not pending (already answered or expired)";
                res.status = 404;
            }
            res.set_content(result.dump(2), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["ok"] = false;
            err["error"] = e.what();
            res.status = 400;
            res.set_content(err.dump(2), "application/json");
        }
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
    if (!running_) {
        return;
    }

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

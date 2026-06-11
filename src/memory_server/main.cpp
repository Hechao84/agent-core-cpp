#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "httplib.h"
#include "include/memory_types.h"
#include "include/resource_manager.h"
#include "src/memory/builtin_memory_runtime.h"
#include "third_party/include/nlohmann/json.hpp"

using namespace jiuwen;

namespace {

std::unique_ptr<BuiltinMemoryRuntime> g_runtime;
std::unique_ptr<Model> g_model;

bool ModelFormatFromString(const std::string& s, ModelFormatType& out)
{
    if (s == "openai")    { out = ModelFormatType::OPENAI; return true; }
    if (s == "anthropic") { out = ModelFormatType::ANTHROPIC; return true; }
    return false;
}

ModelConfig LoadModelConfig(const std::string& jsonFile)
{
    std::ifstream file(jsonFile);
    if (!file.is_open()) {
        return {};
    }
    nlohmann::json j = nlohmann::json::parse(file, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return {};
    }
    ModelConfig cfg;
    if (j.contains("baseUrl"))   cfg.baseUrl   = j["baseUrl"].get<std::string>();
    if (j.contains("apiKey"))    cfg.apiKey    = j["apiKey"].get<std::string>();
    if (j.contains("modelName")) cfg.modelName = j["modelName"].get<std::string>();
    if (j.contains("provider"))  cfg.provider  = j["provider"].get<std::string>();
    if (j.contains("formatType") && j["formatType"].is_string()) {
        ModelFormatType ft;
        if (ModelFormatFromString(j["formatType"].get<std::string>(), ft)) {
            cfg.formatType = ft;
        }
    }
    return cfg;
}

nlohmann::json StatsToJson(const MemoryStats& stats)
{
    nlohmann::json j;
    j["events"] = stats.events;
    j["payloads"] = stats.payloads;
    j["summaries"] = stats.summaries;
    j["entities"] = stats.entities;
    j["relations"] = stats.relations;
    return j;
}

nlohmann::json ContextPackageToJson(const MemoryContextPackage& pkg)
{
    nlohmann::json j;
    j["memoryText"] = pkg.memoryText;
    j["metadata"] = pkg.metadata;
    return j;
}

} // namespace

int main(int argc, char* argv[])
{
    std::string host = "127.0.0.1";
    int port = 8090;
    std::string dataPath = "./data";
    std::string modelConfigPath;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (arg == "--data" && i + 1 < argc) {
            dataPath = argv[++i];
        } else if (arg == "--model-config" && i + 1 < argc) {
            modelConfigPath = argv[++i];
        } else if (arg == "--help") {
            std::printf("memory-server [options]\n"
                        "  --host <ip>             Set server host (default: 127.0.0.1)\n"
                        "  --port <n>              Set server port (default: 8090)\n"
                        "  --data <path>           Set data directory (default: ./data)\n"
                        "  --model-config <path>   Set model config JSON for LLM consolidation\n");
            return 0;
        }
    }

    MemoryConfig config;
    config.dataPath = dataPath;
    config.enablePayloadOffload = true;
    config.offloadToolResultChars = 8000;
    g_runtime = std::make_unique<BuiltinMemoryRuntime>(config);
    if (!modelConfigPath.empty()) {
        ModelConfig modelConfig = LoadModelConfig(modelConfigPath);
        if (!modelConfig.modelName.empty() || !modelConfig.baseUrl.empty()) {
            g_model = ResourceManager::GetInstance().CreateModel(modelConfig);
        }
    }

    httplib::Server server;

    server.Post("/v1/events", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = nlohmann::json::parse(req.body);
            MemoryEvent event;
            event.type = static_cast<MemoryEventType>(j.value("type", 0));
            event.agentId = j.value("agentId", std::string());
            event.sessionId = j.value("sessionId", std::string());
            event.role = j.value("role", std::string());
            event.content = j.value("content", std::string());
            event.toolCallId = j.value("toolCallId", std::string());
            event.toolName = j.value("toolName", std::string());
            event.payloadRef = j.value("payloadRef", std::string());

            bool ok = g_runtime->AppendEvent(event);
            nlohmann::json resp;
            resp["ok"] = ok;
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json resp;
            resp["ok"] = false;
            resp["error"] = e.what();
            res.status = 400;
            res.set_content(resp.dump(), "application/json");
        }
    });

    server.Post("/v1/context", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = nlohmann::json::parse(req.body);
            MemoryContextRequest request;
            request.agentId = j.value("agentId", std::string());
            request.sessionId = j.value("sessionId", std::string());
            request.query = j.value("query", std::string());
            request.tokenBudget = j.value("tokenBudget", 4096);

            auto context = g_runtime->BuildContext(request);
            res.set_content(ContextPackageToJson(context).dump(), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json resp;
            resp["ok"] = false;
            resp["error"] = e.what();
            res.status = 400;
            res.set_content(resp.dump(), "application/json");
        }
    });

    server.Post("/v1/payloads", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = nlohmann::json::parse(req.body);
            MemoryPayloadWriteRequest request;
            request.agentId = j.value("agentId", std::string());
            request.sessionId = j.value("sessionId", std::string());
            request.content = j.value("content", std::string());
            request.contentType = j.value("contentType", std::string());
            request.toolCallId = j.value("toolCallId", std::string());
            request.toolName = j.value("toolName", std::string());

            auto result = g_runtime->WritePayload(request);
            nlohmann::json resp;
            resp["ok"] = true;
            resp["offloaded"] = result.offloaded;
            resp["replacementContent"] = result.replacementContent;
            resp["payload"] = {
                {"ref", result.payload.ref},
                {"contentType", result.payload.contentType},
                {"summary", result.payload.summary},
                {"toolName", result.payload.toolName},
                {"originalChars", result.payload.originalChars}
            };
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json resp;
            resp["ok"] = false;
            resp["error"] = e.what();
            res.status = 400;
            res.set_content(resp.dump(), "application/json");
        }
    });

    server.Get(R"(/v1/payloads/(.*))", [](const httplib::Request& req, httplib::Response& res) {
        std::string ref = "file://" + req.matches[1].str();
        std::string content = g_runtime->ReadPayload(ref);
        nlohmann::json resp;
        resp["ok"] = !content.empty();
        resp["ref"] = ref;
        resp["content"] = content;
        res.set_content(resp.dump(), "application/json");
    });

    server.Post("/v1/consolidate", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = nlohmann::json::parse(req.body);
            MemoryConsolidationRequest request;
            request.agentId = j.value("agentId", std::string());
            request.sessionId = j.value("sessionId", std::string());
            request.maxEvents = j.value("maxEvents", 100);
            request.force = j.value("force", false);

            bool handled = g_runtime->Consolidate(request, g_model.get());
            nlohmann::json resp;
            resp["ok"] = true;
            resp["handled"] = handled;
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json resp;
            resp["ok"] = false;
            resp["error"] = e.what();
            res.status = 400;
            res.set_content(resp.dump(), "application/json");
        }
    });

    server.Post("/v1/search", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = nlohmann::json::parse(req.body);
            MemorySearchRequest request;
            request.agentId = j.value("agentId", std::string());
            request.sessionId = j.value("sessionId", std::string());
            request.query = j.value("query", std::string());
            request.limit = j.value("limit", 10);

            auto results = g_runtime->SearchMemory(request);
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& r : results) {
                nlohmann::json item;
                item["id"] = r.id;
                item["type"] = r.type;
                item["content"] = r.content;
                item["score"] = r.score;
                arr.push_back(item);
            }
            nlohmann::json resp;
            resp["ok"] = true;
            resp["results"] = arr;
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json resp;
            resp["ok"] = false;
            resp["error"] = e.what();
            res.status = 400;
            res.set_content(resp.dump(), "application/json");
        }
    });

    server.Get("/v1/stats", [](const httplib::Request&, httplib::Response& res) {
        auto stats = g_runtime->GetStats();
        res.set_content(StatsToJson(stats).dump(), "application/json");
    });

    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        nlohmann::json resp;
        resp["status"] = "ok";
        res.set_content(resp.dump(), "application/json");
    });

    std::cout << "memory-server starting on " << host << ":" << port
              << " data=" << dataPath
              << " model=" << (g_model ? "enabled" : "disabled") << std::endl;

    if (!server.listen(host, port)) {
        std::cerr << "Failed to listen on " << host << ":" << port << std::endl;
        return 1;
    }
    return 0;
}

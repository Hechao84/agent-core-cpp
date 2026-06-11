#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

nlohmann::json MakeTextResult(const nlohmann::json& value)
{
    nlohmann::json result;
    result["content"] = nlohmann::json::array();
    nlohmann::json item;
    item["type"] = "text";
    item["text"] = value.dump();
    result["content"].push_back(item);
    return result;
}

nlohmann::json Error(int id, int code, const std::string& message)
{
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["error"] = {{"code", code}, {"message", message}};
    return resp;
}

nlohmann::json Success(int id, const nlohmann::json& result)
{
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"] = result;
    return resp;
}

nlohmann::json ToolSchema(const std::string& name, const std::string& description, const nlohmann::json& properties,
                          const std::vector<std::string>& required)
{
    nlohmann::json tool;
    tool["name"] = name;
    tool["description"] = description;
    tool["inputSchema"] = {
        {"type", "object"},
        {"properties", properties},
        {"required", required},
    };
    return tool;
}

nlohmann::json ListTools()
{
    nlohmann::json tools = nlohmann::json::array();
    tools.push_back(ToolSchema("memory_append_event", "Append a memory event.",
                               {{"type", {{"type", "integer"}}},
                                {"agentId", {{"type", "string"}}},
                                {"sessionId", {{"type", "string"}}},
                                {"role", {{"type", "string"}}},
                                {"content", {{"type", "string"}}},
                                {"toolCallId", {{"type", "string"}}},
                                {"toolName", {{"type", "string"}}},
                                {"payloadRef", {{"type", "string"}}}},
                               {"type", "agentId", "sessionId"}));
    tools.push_back(ToolSchema("memory_build_context", "Build memory context.",
                               {{"agentId", {{"type", "string"}}},
                                {"sessionId", {{"type", "string"}}},
                                {"query", {{"type", "string"}}},
                                {"tokenBudget", {{"type", "integer"}}}},
                               {"agentId", "sessionId"}));
    tools.push_back(ToolSchema("memory_read_payload", "Read an offloaded payload.",
                               {{"ref", {{"type", "string"}}}}, {"ref"}));
    tools.push_back(ToolSchema("memory_consolidate", "Trigger memory consolidation.",
                               {{"agentId", {{"type", "string"}}},
                                {"sessionId", {{"type", "string"}}},
                                {"maxEvents", {{"type", "integer"}}},
                                {"force", {{"type", "boolean"}}}},
                               {"agentId"}));
    tools.push_back(ToolSchema("memory_search", "Search memory.",
                               {{"agentId", {{"type", "string"}}},
                                {"sessionId", {{"type", "string"}}},
                                {"query", {{"type", "string"}}},
                                {"limit", {{"type", "integer"}}}},
                               {"query"}));
    tools.push_back(ToolSchema("memory_stats", "Return memory stats.", nlohmann::json::object(), {}));

    return {{"tools", tools}};
}

nlohmann::json StatsToJson(const MemoryStats& stats)
{
    return {{"events", stats.events},
            {"payloads", stats.payloads},
            {"summaries", stats.summaries},
            {"entities", stats.entities},
            {"relations", stats.relations}};
}

nlohmann::json CallTool(const std::string& name, const nlohmann::json& args)
{
    if (name == "memory_append_event") {
        MemoryEvent event;
        event.type = static_cast<MemoryEventType>(args.value("type", 0));
        event.agentId = args.value("agentId", std::string());
        event.sessionId = args.value("sessionId", std::string());
        event.role = args.value("role", std::string());
        event.content = args.value("content", std::string());
        event.toolCallId = args.value("toolCallId", std::string());
        event.toolName = args.value("toolName", std::string());
        event.payloadRef = args.value("payloadRef", std::string());
        return MakeTextResult({{"ok", g_runtime->AppendEvent(event)}});
    }

    if (name == "memory_build_context") {
        MemoryContextRequest request;
        request.agentId = args.value("agentId", std::string());
        request.sessionId = args.value("sessionId", std::string());
        request.query = args.value("query", std::string());
        request.tokenBudget = args.value("tokenBudget", 4096);
        auto context = g_runtime->BuildContext(request);
        return MakeTextResult({{"memoryText", context.memoryText}, {"metadata", context.metadata}});
    }

    if (name == "memory_read_payload") {
        std::string ref = args.value("ref", std::string());
        std::string content = g_runtime->ReadPayload(ref);
        return MakeTextResult({{"ok", !content.empty()}, {"ref", ref}, {"content", content}});
    }

    if (name == "memory_consolidate") {
        MemoryConsolidationRequest request;
        request.agentId = args.value("agentId", std::string());
        request.sessionId = args.value("sessionId", std::string());
        request.maxEvents = args.value("maxEvents", 100);
        request.force = args.value("force", false);
        return MakeTextResult({{"ok", true}, {"handled", g_runtime->Consolidate(request, g_model.get())}});
    }

    if (name == "memory_search") {
        MemorySearchRequest request;
        request.agentId = args.value("agentId", std::string());
        request.sessionId = args.value("sessionId", std::string());
        request.query = args.value("query", std::string());
        request.limit = args.value("limit", 10);
        auto results = g_runtime->SearchMemory(request);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : results) {
            arr.push_back({{"id", r.id}, {"type", r.type}, {"content", r.content}, {"score", r.score}});
        }
        return MakeTextResult({{"ok", true}, {"results", arr}});
    }

    if (name == "memory_stats") {
        return MakeTextResult(StatsToJson(g_runtime->GetStats()));
    }

    return MakeTextResult({{"ok", false}, {"error", "unknown tool: " + name}});
}

nlohmann::json HandleRequest(const nlohmann::json& req)
{
    int id = req.value("id", 0);
    std::string method = req.value("method", std::string());

    if (method == "initialize") {
        nlohmann::json result;
        result["protocolVersion"] = "2024-11-05";
        result["serverInfo"] = {{"name", "memory-mcp-server"}, {"version", "0.1.0"}};
        result["capabilities"] = {{"tools", nlohmann::json::object()}};
        return Success(id, result);
    }

    if (method == "tools/list") {
        return Success(id, ListTools());
    }

    if (method == "tools/call") {
        auto params = req.value("params", nlohmann::json::object());
        std::string name = params.value("name", std::string());
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        return Success(id, CallTool(name, args));
    }

    return Error(id, -32601, "method not found: " + method);
}

} // namespace

int main(int argc, char* argv[])
{
    std::string dataPath = "./data";
    std::string modelConfigPath;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data" && i + 1 < argc) {
            dataPath = argv[++i];
        } else if (arg == "--model-config" && i + 1 < argc) {
            modelConfigPath = argv[++i];
        }
    }

    MemoryConfig config;
    config.dataPath = dataPath;
    config.enablePayloadOffload = true;
    g_runtime = std::make_unique<BuiltinMemoryRuntime>(config);
    if (!modelConfigPath.empty()) {
        ModelConfig modelConfig = LoadModelConfig(modelConfigPath);
        if (!modelConfig.modelName.empty() || !modelConfig.baseUrl.empty()) {
            g_model = ResourceManager::GetInstance().CreateModel(modelConfig);
        }
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        try {
            nlohmann::json req = nlohmann::json::parse(line);
            std::cout << HandleRequest(req).dump() << std::endl;
        } catch (const std::exception& e) {
            std::cout << Error(0, -32700, e.what()).dump() << std::endl;
        }
    }
    return 0;
}

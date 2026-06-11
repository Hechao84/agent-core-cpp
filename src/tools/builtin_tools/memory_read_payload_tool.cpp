#include "src/tools/builtin_tools/memory_read_payload_tool.h"

#include "include/memory_runtime.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

namespace {

std::string MakeError(const std::string& msg)
{
    nlohmann::json j;
    j["ok"] = false;
    j["error"] = msg;
    return j.dump();
}

std::string MakeOk(const std::string& ref, const std::string& content)
{
    nlohmann::json j;
    j["ok"] = true;
    j["ref"] = ref;
    j["content"] = content;
    j["chars"] = static_cast<int>(content.size());
    return j.dump();
}

} // namespace

MemoryReadPayloadTool::MemoryReadPayloadTool(MemoryRuntime* memoryRuntime)
    : Tool("memory_read_payload",
           "Read full content from an offloaded memory payload reference. Input: JSON {\"ref\": \"file://...\"}.",
           {{"ref", "Payload reference returned in a tool message payload_ref", "string", true}}),
      memoryRuntime_(memoryRuntime)
{
}

std::string MemoryReadPayloadTool::Invoke(const std::string& input)
{
    if (memoryRuntime_ == nullptr) {
        return MakeError("memory runtime is not available in this context");
    }

    std::string ref;
    try {
        auto j = nlohmann::json::parse(input);
        if (!j.contains("ref") || !j["ref"].is_string()) {
            return MakeError("'ref' string is required");
        }
        ref = j["ref"].get<std::string>();
    } catch (const std::exception& e) {
        return MakeError(std::string("invalid JSON: ") + e.what());
    }

    std::string content = memoryRuntime_->ReadPayload(ref);
    if (content.empty()) {
        return MakeError("payload not found or empty: " + ref);
    }
    return MakeOk(ref, content);
}

} // namespace jiuwen

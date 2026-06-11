#include "src/tools/builtin_tools/ask_user_tool.h"

#include <atomic>
#include <chrono>
#include <random>
#include <sstream>
#include <string>

#include "src/core/ask_user_dispatcher.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

namespace {

constexpr int kAskUserTimeoutSec = 60;

std::string GenerateRequestId()
{
    static std::atomic<uint64_t> counter{0};
    uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);
    uint64_t rand = std::random_device{}() ^ static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::ostringstream oss;
    oss << "ask-" << std::hex << seq << "-" << rand;
    return oss.str();
}

std::string MakeError(const std::string& msg)
{
    nlohmann::json j;
    j["ok"] = false;
    j["error"] = msg;
    return j.dump();
}

} // namespace

AskUserTool::AskUserTool(AskUserDispatcher* dispatcher, StreamCallback streamCallback)
    : Tool("ask_user",
           "Ask the end user a structured question and wait up to 60s for their answer. "
           "Input: JSON {\"question\": <string required>, \"options\": [<string>...] optional, "
           "\"multiple\": <bool optional, default false>, \"header\": <string optional short label>}. "
           "Returns the user's answer (free text, or comma-joined selected option labels). "
           "Returns empty string on timeout.",
           {{"question", "The question to ask the user", "string", true},
            {"options", "Optional list of choices presented to the user", "array", false},
            {"multiple", "Allow selecting multiple options (default false)", "boolean", false},
            {"header", "Short header/label (max 30 chars)", "string", false}}),
      dispatcher_(dispatcher),
      streamCallback_(std::move(streamCallback)) {}

std::string AskUserTool::Invoke(const std::string& input)
{
    if (dispatcher_ == nullptr) {
        return MakeError("ask_user is not available in this context (no dispatcher)");
    }
    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(input);
    } catch (const std::exception& e) {
        return MakeError(std::string("invalid JSON: ") + e.what());
    }
    if (!payload.contains("question") || !payload["question"].is_string()) {
        return MakeError("'question' (string) is required");
    }

    std::string requestId = GenerateRequestId();
    payload["request_id"] = requestId;
    if (!payload.contains("timeout_seconds")) {
        payload["timeout_seconds"] = kAskUserTimeoutSec;
    }

    dispatcher_->EmitAskUser(requestId, payload.dump(), streamCallback_);
    auto answer = dispatcher_->WaitForResponse(requestId, std::chrono::seconds(kAskUserTimeoutSec));
    if (!answer.has_value()) {
        // Timeout: framework contract is to return empty string so the LLM
        // can decide how to proceed without the user's input.
        return "";
    }
    return *answer;
}

} // namespace jiuwen

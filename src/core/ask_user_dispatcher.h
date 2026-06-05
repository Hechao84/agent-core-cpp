#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace jiuwen {

class AskUserDispatcher {
public:
    using StreamCallback = std::function<void(const std::string&)>;

    AskUserDispatcher() = default;

    // Called by the AskUserTool: emit a [ASK_USER]...[/ASK_USER] tag via the
    // streaming callback and register a slot that ProvideResponse can wake.
    void EmitAskUser(const std::string& requestId, const std::string& payloadJson,
                     const StreamCallback& streamCallback);

    // Block until a response arrives via ProvideResponse or the timeout fires.
    // Returns std::nullopt on timeout.
    std::optional<std::string> WaitForResponse(const std::string& requestId,
                                                std::chrono::seconds timeout);

    // Called by the application layer (HTTP / CLI). Returns true if the
    // request id matched a pending slot.
    bool ProvideResponse(const std::string& requestId, const std::string& answer);

private:
    struct Slot {
        std::mutex m;
        std::condition_variable cv;
        std::optional<std::string> answer;
        bool done{false};
    };

    std::shared_ptr<Slot> GetOrCreateSlot(const std::string& requestId);
    std::shared_ptr<Slot> TakeSlot(const std::string& requestId);

    mutable std::mutex slotsMu_;
    std::unordered_map<std::string, std::shared_ptr<Slot>> slots_;
};

} // namespace jiuwen

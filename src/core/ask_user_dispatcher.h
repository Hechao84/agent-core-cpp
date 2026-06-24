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

// Interface for registering/unregistering requestId→sessionId mappings.
// Implemented by SessionManager so AskUserDispatcher can inform it when
// an ask request starts and ends.
class AskUserRouter {
public:
    virtual ~AskUserRouter() = default;
    virtual void RegisterAskRequest(const std::string& requestId, const std::string& sessionId) = 0;
    virtual void UnregisterAskRequest(const std::string& requestId) = 0;
};

class AskUserDispatcher {
public:
    using StreamCallback = std::function<void(const std::string&)>;

    // Default constructor for standalone use (unit tests, etc.).
    AskUserDispatcher() = default;

    // Constructor with sessionId and router for production use.
    // The router is called on EmitAskUser (register) and when the
    // request completes or times out (unregister), so SessionManager
    // can maintain a requestId→sessionId index for routing answers.
    AskUserDispatcher(const std::string& sessionId, AskUserRouter* router)
        : sessionId_(sessionId), router_(router) {}

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

    std::string sessionId_;
    AskUserRouter* router_{nullptr};
};

} // namespace jiuwen

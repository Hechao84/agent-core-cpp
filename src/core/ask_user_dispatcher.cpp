#include "src/core/ask_user_dispatcher.h"

namespace jiuwen {

std::shared_ptr<AskUserDispatcher::Slot> AskUserDispatcher::GetOrCreateSlot(const std::string& requestId)
{
    std::lock_guard<std::mutex> lock(slotsMu_);
    auto it = slots_.find(requestId);
    if (it != slots_.end()) {
        return it->second;
    }
    auto slot = std::make_shared<Slot>();
    slots_[requestId] = slot;
    return slot;
}

std::shared_ptr<AskUserDispatcher::Slot> AskUserDispatcher::TakeSlot(const std::string& requestId)
{
    std::lock_guard<std::mutex> lock(slotsMu_);
    auto it = slots_.find(requestId);
    if (it == slots_.end()) {
        return nullptr;
    }
    auto slot = it->second;
    slots_.erase(it);
    return slot;
}

void AskUserDispatcher::EmitAskUser(const std::string& requestId, const std::string& payloadJson,
                                     const StreamCallback& streamCallback)
{
    GetOrCreateSlot(requestId);
    if (router_) {
        router_->RegisterAskRequest(requestId, sessionId_);
    }
    if (streamCallback) {
        std::string tag = "\n[ASK_USER]" + payloadJson + "[/ASK_USER]\n";
        streamCallback(tag);
    }
}

std::optional<std::string> AskUserDispatcher::WaitForResponse(const std::string& requestId,
                                                                std::chrono::seconds timeout)
{
    auto slot = GetOrCreateSlot(requestId);
    std::unique_lock<std::mutex> lock(slot->m);
    bool ok = slot->cv.wait_for(lock, timeout, [&slot]() { return slot->done; });
    std::optional<std::string> result;
    if (ok && slot->answer.has_value()) {
        result = slot->answer;
    }
    lock.unlock();

    // Always remove the slot once we are done waiting, whether or not we got
    // an answer. A late ProvideResponse for this request id becomes a no-op.
    {
        std::lock_guard<std::mutex> sl(slotsMu_);
        slots_.erase(requestId);
    }
    if (router_) {
        router_->UnregisterAskRequest(requestId);
    }
    return result;
}

bool AskUserDispatcher::ProvideResponse(const std::string& requestId, const std::string& answer)
{
    std::shared_ptr<Slot> slot;
    {
        std::lock_guard<std::mutex> lock(slotsMu_);
        auto it = slots_.find(requestId);
        if (it == slots_.end()) {
            return false;
        }
        slot = it->second;
    }
    {
        std::lock_guard<std::mutex> sl(slot->m);
        slot->answer = answer;
        slot->done = true;
    }
    slot->cv.notify_all();
    return true;
}

} // namespace jiuwen

#include "src/context_engine/context_engine.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "src/context_engine/db_storage.h"
#include "src/context_engine/json_storage.h"
#include "src/context_engine/storage_base.h"
#include "src/utils/data_dir.h"
#include "src/utils/logger.h"

namespace fs = std::filesystem;

namespace jiuwen {

struct ContextEngine::MessageSegment
{
    size_t start{0};
    size_t end{0};
    bool startsWithUser{false};
    int tokens{0};
};

ContextEngine::ContextEngine(const ContextConfig& config)
    : config_(config)
{
}

ContextEngine::~ContextEngine() = default;

bool ContextEngine::Initialize()
{
    switch (config_.storageType) {
        case ContextConfig::StorageType::DATABASE:
            storage_ = std::make_unique<DbStorage>(config_.storagePath, config_.sessionId);
            break;
        case ContextConfig::StorageType::JSON_FILE:
            storage_ = std::make_unique<JsonStorage>(config_.storagePath, config_.sessionId);
            break;
        case ContextConfig::StorageType::MEMORY_ONLY:
        default:
            storage_ = nullptr;
            break;
    }

    if (storage_) {
        if (!storage_->LoadHistory(memoryBuffer_)) {
            return false;
        }
        // Defensive: drop trailing assistant(tool_calls) with no matching
        // tool response. This guards against histories left in a broken
        // state by a previous interrupted run.
        size_t before = memoryBuffer_.size();
        TrimOrphanTrailingToolCalls(memoryBuffer_);
        if (memoryBuffer_.size() < before) {
            LOG(WARN) << "[ContextEngine] Dropped " << (before - memoryBuffer_.size())
                      << " trailing message(s) with orphan tool_calls in session=" << config_.sessionId;
        }
    }
    return true;
}

void ContextEngine::AddMessage(const Message& message)
{
    if (!ContextStorageBase::IsValidMessage(message)) return;
    memoryBuffer_.push_back(message);
    if (storage_) {
        storage_->SaveMessage(message);
    }
    if (memoryEventSink_) {
        MemoryEvent event;
        event.type = MemoryEventType::MESSAGE_APPENDED;
        event.sessionId = config_.sessionId;
        event.role = message.role;
        event.content = message.content;
        event.toolCallId = message.toolCallId;
        event.toolName = message.toolName;
        event.payloadRef = message.payloadRef;
        memoryEventSink_(event);
    }
}

std::vector<Message> ContextEngine::GetContextWindow() const
{
    if (memoryBuffer_.empty()) return {};
    return ApplyContextLimits(memoryBuffer_);
}

int ContextEngine::CalculateMessageTokens(const Message& msg) const
{
    int total = EstimateTokens(msg.role) + EstimateTokens(msg.content);
    for (const auto& tc : msg.toolCalls) {
        total += EstimateTokens(tc.name) + EstimateTokens(tc.argumentsJson);
    }
    total += EstimateTokens(msg.toolCallId) + EstimateTokens(msg.toolName) + EstimateTokens(msg.payloadRef);
    return total;
}

bool ContextEngine::CanMerge(const Message& prev, const Message& cur)
{
    if (prev.role != cur.role) return false;
    // tool messages must each retain their own tool_call_id
    if (prev.role == "tool") return false;
    // assistant carrying tool_calls is structural
    if (prev.role == "assistant" && (!prev.toolCalls.empty() || !cur.toolCalls.empty())) {
        return false;
    }
    // user / system / plain assistant text: safe to merge
    return true;
}

int ContextEngine::CalculateMessagesTokens(const std::vector<Message>& messages) const
{
    int total = 0;
    for (const auto& message : messages) {
        total += CalculateMessageTokens(message);
    }
    return total;
}

std::vector<ContextEngine::MessageSegment> ContextEngine::BuildMessageSegments(const std::vector<Message>& messages) const
{
    std::vector<MessageSegment> segments;
    if (messages.empty()) return segments;

    size_t start = 0;
    for (size_t i = 1; i < messages.size(); ++i) {
        if (messages[i].role == "user") {
            MessageSegment segment;
            segment.start = start;
            segment.end = i;
            segment.startsWithUser = messages[start].role == "user";
            segment.tokens = CalculateMessagesTokens(std::vector<Message>(messages.begin() + start, messages.begin() + i));
            segments.push_back(segment);
            start = i;
        }
    }

    MessageSegment segment;
    segment.start = start;
    segment.end = messages.size();
    segment.startsWithUser = messages[start].role == "user";
    segment.tokens = CalculateMessagesTokens(std::vector<Message>(messages.begin() + start, messages.end()));
    segments.push_back(segment);
    return segments;
}

std::vector<Message> ContextEngine::CompressSegment(
    const std::vector<Message>& messages,
    const MessageSegment& segment,
    int tokenBudget) const
{
    if (segment.start >= segment.end || segment.end > messages.size() || tokenBudget <= 0) return {};

    std::vector<Message> segmentMessages(messages.begin() + segment.start, messages.begin() + segment.end);
    if (CalculateMessagesTokens(segmentMessages) <= tokenBudget &&
        static_cast<int>(segmentMessages.size()) <= config_.maxMessages) {
        return segmentMessages;
    }

    std::vector<Message> compressed;
    const Message& first = segmentMessages.front();
    if (first.role == "user") {
        compressed.push_back(first);
    }

    std::vector<std::string> toolSummaries;
    std::string lastAssistantText;
    for (const auto& message : segmentMessages) {
        if (message.role == "tool") {
            std::string summary = "- " + (message.toolName.empty() ? "tool" : message.toolName);
            if (!message.payloadRef.empty()) {
                summary += " payload_ref=" + message.payloadRef;
            }
            if (!message.content.empty()) {
                std::string preview = message.content.substr(0, 240);
                if (message.content.size() > preview.size()) preview += "...";
                summary += ": " + preview;
            }
            toolSummaries.push_back(summary);
        } else if (message.role == "assistant" && message.toolCalls.empty() && !message.content.empty()) {
            lastAssistantText = message.content;
        }
    }

    if (!toolSummaries.empty() || !lastAssistantText.empty()) {
        Message summary;
        summary.role = "assistant";
        summary.content = "Context segment compressed due to context window limits.";
        if (!toolSummaries.empty()) {
            summary.content += "\n\nTool results summary:";
            int kept = 0;
            for (const auto& item : toolSummaries) {
                if (kept >= 8) break;
                summary.content += "\n" + item;
                ++kept;
            }
            if (static_cast<int>(toolSummaries.size()) > kept) {
                summary.content += "\n- ...";
            }
        }
        if (!lastAssistantText.empty()) {
            std::string preview = lastAssistantText.substr(0, 800);
            if (lastAssistantText.size() > preview.size()) preview += "...";
            summary.content += "\n\nLatest assistant state:\n" + preview;
        }
        compressed.push_back(summary);
    }

    while (CalculateMessagesTokens(compressed) > tokenBudget && compressed.size() > 1) {
        compressed.pop_back();
    }
    return compressed;
}

std::vector<Message> ContextEngine::ApplyContextLimits(const std::vector<Message>& messages) const
{
    if (messages.empty()) return {};

    std::vector<Message> sanitized;
    sanitized.reserve(messages.size());
    for (const auto& msg : messages) {
        if (!sanitized.empty() && CanMerge(sanitized.back(), msg)) {
            sanitized.back().content = MergeMessageContent(sanitized.back().content, msg.content);
        } else {
            sanitized.push_back(msg);
        }
    }

    auto segments = BuildMessageSegments(sanitized);
    if (segments.empty()) return {};

    std::vector<std::vector<Message>> selectedReversed;
    int totalTokens = 0;
    int selectedSegments = 0;
    bool compressedCurrentSegment = false;

    for (int i = static_cast<int>(segments.size()) - 1; i >= 0; --i) {
        const auto& segment = segments[i];
        std::vector<Message> segmentMessages(sanitized.begin() + segment.start, sanitized.begin() + segment.end);
        int segmentTokens = CalculateMessagesTokens(segmentMessages);
        bool isLatest = i == static_cast<int>(segments.size()) - 1;
        bool fitsMessages = static_cast<int>(segmentMessages.size()) <= config_.maxMessages;
        bool fitsTokens = totalTokens + segmentTokens <= config_.maxContextTokens;
        bool fits = fitsMessages && fitsTokens;

        if (fits) {
            selectedReversed.push_back(segmentMessages);
            totalTokens += segmentTokens;
            ++selectedSegments;
            continue;
        }

        if (isLatest) {
            int budget = std::max(config_.maxContextTokens - totalTokens, 1);
            auto compressed = CompressSegment(sanitized, segment, budget);
            if (!compressed.empty()) {
                selectedReversed.push_back(compressed);
                totalTokens += CalculateMessagesTokens(compressed);
                ++selectedSegments;
                compressedCurrentSegment = true;
            }
        }
        break;
    }

    std::vector<Message> result;
    for (auto it = selectedReversed.rbegin(); it != selectedReversed.rend(); ++it) {
        result.insert(result.end(), it->begin(), it->end());
    }

    while (static_cast<int>(result.size()) > config_.maxMessages && result.size() > 1) {
        result.erase(result.begin());
    }

    int droppedUnpairedToolMessages = DropUnpairedToolMessages(result);
    int assistantToolCallCount = 0;
    int toolResultCount = 0;
    for (const auto& msg : result) {
        if (msg.role == "assistant") {
            assistantToolCallCount += static_cast<int>(msg.toolCalls.size());
        } else if (msg.role == "tool") {
            ++toolResultCount;
        }
    }

    LOG(INFO) << "[ContextWindow] sessionId=" << config_.sessionId
              << " totalMessages=" << messages.size()
              << " sanitizedMessages=" << sanitized.size()
              << " totalSegments=" << segments.size()
              << " selectedSegments=" << selectedSegments
              << " droppedSegments=" << (segments.size() - selectedSegments)
              << " compressedCurrentSegment=" << (compressedCurrentSegment ? "true" : "false")
              << " droppedUnpairedToolMessages=" << droppedUnpairedToolMessages
              << " assistantToolCalls=" << assistantToolCallCount
              << " toolResults=" << toolResultCount
              << " finalMessages=" << result.size()
              << " finalTokens=" << CalculateMessagesTokens(result)
              << " maxMessages=" << config_.maxMessages
              << " maxContextTokens=" << config_.maxContextTokens;

    return result;
}

std::string ContextEngine::GetContextAsString() const
{
    auto messages = GetContextWindow();
    if (messages.empty()) return "";
    std::ostringstream oss;
    for (const auto& msg : messages) {
        oss << msg.role;
        if (!msg.toolCalls.empty()) {
            oss << " (tool_calls=" << msg.toolCalls.size() << ")";
        }
        if (!msg.toolCallId.empty()) {
            oss << " (tool_call_id=" << msg.toolCallId << ")";
        }
        if (!msg.payloadRef.empty()) {
            oss << " (payload_ref=" << msg.payloadRef << ")";
        }
        oss << ": " << msg.content << "\n";
    }
    return oss.str();
}

std::vector<Message> ContextEngine::GetAllMessages() const
{
    return memoryBuffer_;
}

void ContextEngine::Clear()
{
    memoryBuffer_.clear();
    if (storage_) {
        storage_->Clear();
    }
}

int ContextEngine::GetTokenCount() const
{
    int total = 0;
    for (const auto& msg : memoryBuffer_) {
        total += CalculateMessageTokens(msg);
    }
    return total;
}

std::string ContextEngine::GetSessionId() const
{
    return config_.sessionId;
}

int ContextEngine::EstimateTokens(const std::string& text)
{
    return static_cast<int>(text.length()) / 4;
}

std::string ContextEngine::LoadMemoryContext() const
{
    std::lock_guard<std::mutex> lock(memoryMutex_);
    fs::path memoryPath = fs::path(GetDataDir().GetMemoryPath());
    if (fs::exists(memoryPath) && fs::is_regular_file(memoryPath)) {
        std::ifstream file(memoryPath);
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }
    }
    return "";
}

std::string ContextEngine::MergeMessageContent(const std::string& left, const std::string& right)
{
    if (left.empty()) return right;
    if (right.empty()) return left;
    return left + "\n\n" + right;
}

int ContextEngine::DropUnpairedToolMessages(std::vector<Message>& msgs)
{
    int dropped = 0;
    dropped += DropOrphanToolCalls(msgs);

    std::unordered_set<std::string> assistantCallIds;
    for (const auto& msg : msgs) {
        if (msg.role == "assistant") {
            for (const auto& tc : msg.toolCalls) {
                if (!tc.id.empty()) assistantCallIds.insert(tc.id);
            }
        }
    }

    std::vector<Message> cleaned;
    cleaned.reserve(msgs.size());
    for (auto msg : msgs) {
        if (msg.role == "tool" && assistantCallIds.find(msg.toolCallId) == assistantCallIds.end()) {
            ++dropped;
            continue;
        }
        cleaned.push_back(std::move(msg));
    }
    msgs.swap(cleaned);
    return dropped;
}

int ContextEngine::DropOrphanToolCalls(std::vector<Message>& msgs)
{
    std::unordered_set<std::string> toolResultIds;
    for (const auto& msg : msgs) {
        if (msg.role == "tool" && !msg.toolCallId.empty()) {
            toolResultIds.insert(msg.toolCallId);
        }
    }

    int dropped = 0;
    std::vector<Message> cleaned;
    cleaned.reserve(msgs.size());
    for (auto msg : msgs) {
        if (msg.role == "assistant" && !msg.toolCalls.empty()) {
            std::vector<ToolCall> kept;
            kept.reserve(msg.toolCalls.size());
            for (const auto& tc : msg.toolCalls) {
                if (!tc.id.empty() && toolResultIds.find(tc.id) != toolResultIds.end()) {
                    kept.push_back(tc);
                } else {
                    ++dropped;
                }
            }
            msg.toolCalls = std::move(kept);
            if (msg.toolCalls.empty() && msg.content.empty()) {
                continue;
            }
        }
        cleaned.push_back(std::move(msg));
    }
    msgs.swap(cleaned);
    return dropped;
}

void ContextEngine::TrimOrphanTrailingToolCalls(std::vector<Message>& msgs)
{
    while (!msgs.empty()) {
        const auto& last = msgs.back();
        if (last.role == "assistant" && !last.toolCalls.empty()) {
            msgs.pop_back();
        } else {
            break;
        }
    }
}

std::vector<Message> ContextEngine::BuildMessagesForLLM(
    const std::string& systemPrompt,
    const std::vector<Message>& history,
    const Message& currentMessage) const
{
    std::vector<Message> result;
    if (!systemPrompt.empty()) {
        Message sys;
        sys.role = "system";
        sys.content = systemPrompt;
        result.push_back(sys);
    }
    auto limitedHistory = ApplyContextLimits(history);
    result.insert(result.end(), limitedHistory.begin(), limitedHistory.end());
    if (!result.empty() && CanMerge(result.back(), currentMessage)) {
        result.back().content = MergeMessageContent(result.back().content, currentMessage.content);
    } else {
        result.push_back(currentMessage);
    }
    return result;
}

std::string ContextEngine::GetMemoryContent() const
{
    if (memoryContextProvider_) {
        try {
            std::string content = memoryContextProvider_();
            if (!content.empty()) {
                return content;
            }
        } catch (const std::exception& e) {
            LOG(WARN) << "[ContextEngine] Memory context provider failed: " << e.what();
        } catch (...) {
            LOG(WARN) << "[ContextEngine] Memory context provider failed";
        }
    }
    return LoadMemoryContext();
}

void ContextEngine::SetMemoryContextProvider(std::function<std::string()> provider)
{
    memoryContextProvider_ = std::move(provider);
}

void ContextEngine::SetMemoryEventSink(std::function<void(const MemoryEvent&)> sink)
{
    memoryEventSink_ = std::move(sink);
}

std::string ContextEngine::GetConsolidationPayload(int maxMessages) const
{
    if (memoryBuffer_.empty()) return "";
    int startIdx = 0;
    if (static_cast<int>(memoryBuffer_.size()) > maxMessages) {
        startIdx = static_cast<int>(memoryBuffer_.size()) - maxMessages;
    }
    std::string result;
    for (int i = startIdx; i < static_cast<int>(memoryBuffer_.size()); ++i) {
        const auto& m = memoryBuffer_[i];
        result += m.role + ": " + m.content + "\n\n";
    }
    return result;
}

} // namespace jiuwen

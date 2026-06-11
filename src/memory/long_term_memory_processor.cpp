#include "src/memory/long_term_memory_processor.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {


std::string RuleBasedLongTermMemoryProcessor::ToLower(const std::string& text) const
{
    std::string result = text;
    for (char& ch : result) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return result;
}

bool RuleBasedLongTermMemoryProcessor::ContainsPreferenceSignal(const std::string& text) const
{
    std::string lower = ToLower(text);
    return lower.find("i like") != std::string::npos ||
           lower.find("i prefer") != std::string::npos ||
           lower.find("i want") != std::string::npos ||
           lower.find("please") != std::string::npos ||
           lower.find("always") != std::string::npos ||
           lower.find("never") != std::string::npos;
}

std::string RuleBasedLongTermMemoryProcessor::DetectTopic(const std::string& text) const
{
    std::string lower = ToLower(text);
    std::vector<std::pair<std::string, std::string>> patterns = {
        {"project", "project"},
        {"repository", "project"},
        {"code", "coding"},
        {"api", "api"},
        {"test", "testing"},
        {"deploy", "deployment"},
        {"database", "database"},
        {"config", "configuration"},
        {"security", "security"},
        {"ui", "ui"},
        {"frontend", "ui"},
        {"backend", "backend"},
        {"memory", "memory_system"},
    };
    for (const auto& pattern : patterns) {
        if (lower.find(pattern.first) != std::string::npos) {
            return pattern.second;
        }
    }
    return "";
}

LongTermMemoryUpdate RuleBasedLongTermMemoryProcessor::Process(const LongTermMemoryBatch& batch)
{
    LongTermMemoryUpdate update;
    std::set<std::string> seenTopics;
    bool hasPreference = false;

    for (const auto& event : batch.events) {
        if (event.type != MemoryEventType::MESSAGE_APPENDED) {
            continue;
        }

        std::string topic = DetectTopic(event.content);
        if (!topic.empty() && seenTopics.find(topic) == seenTopics.end()) {
            seenTopics.insert(topic);
            std::string topicSummary = "Discussed topic: " + topic;
            update.topicSummaries.push_back(topicSummary);

            if (topic != "memory_system") {
                MemoryEntity entity;
                entity.id = "entity:topic." + topic;
                entity.type = "topic";
                entity.name = topic + " topic";
                entity.summary = topicSummary;
                entity.confidence = 0.5F;
                entity.sourceRefs.push_back("event://consolidation");
                update.entities.push_back(std::move(entity));
            }
        }

        if (!hasPreference && event.role == "user" && ContainsPreferenceSignal(event.content)) {
            hasPreference = true;
            MemoryEntity entity;
            entity.id = "entity:preference.user";
            entity.type = "preference";
            entity.name = "User preference";
            entity.summary = "User expressed preferences in session";
            entity.confidence = 0.6F;
            entity.sourceRefs.push_back("event://consolidation");
            update.entities.push_back(std::move(entity));

            MemoryRelation relation;
            relation.fromEntity = "entity:user";
            relation.relation = "has_preference";
            relation.toEntity = "entity:preference.user";
            relation.confidence = 0.5F;
            relation.sourceRefs.push_back("event://consolidation");
            update.relations.push_back(std::move(relation));
        }
    }

    return update;
}


LLMLongTermMemoryProcessor::LLMLongTermMemoryProcessor(Model* model)
    : model_(model)
{
}

std::string LLMLongTermMemoryProcessor::BuildPrompt(const LongTermMemoryBatch& batch) const
{
    std::stringstream prompt;
    prompt << R"(Extract structured long-term memory from the conversation events below.
Return ONLY valid JSON with this exact schema (no markdown fences, no explanation):

{
  "topicSummaries": ["brief topic summary 1", "brief topic summary 2"],
  "profileSummaries": ["user profile insight 1"],
  "entities": [
    {
      "id": "entity:unique.id.here",
      "type": "preference|topic|project|task|file|user|style",
      "name": "short name",
      "summary": "one-line summary",
      "confidence": 0.0-1.0,
      "sourceRefs": ["event://1"]
    }
  ],
  "relations": [
    {
      "fromEntity": "entity:user",
      "relation": "prefers|works_on|mentions|related_to|supersedes|contradicts",
      "toEntity": "entity:...",
      "confidence": 0.0-1.0,
      "sourceRefs": ["event://1"]
    }
  ]
}

Use "supersedes" when new information replaces an older entity.
Use "contradicts" when information conflicts with a previous entity.
Use "prefers" for user preferences.

Conversation events:
)";

    for (size_t i = 0; i < batch.events.size(); ++i) {
        const auto& event = batch.events[i];
        prompt << "event://" << (i + 1) << " [" << event.role << "]: " << event.content << "\n";
    }

    return prompt.str();
}

LongTermMemoryUpdate LLMLongTermMemoryProcessor::ParseResponse(
    const std::string& jsonStr, const std::vector<std::string>& sourceRefs) const
{
    LongTermMemoryUpdate update;
    auto json = nlohmann::json::parse(jsonStr, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return update;
    }

    if (json.contains("topicSummaries") && json["topicSummaries"].is_array()) {
        for (const auto& item : json["topicSummaries"]) {
            if (item.is_string()) {
                update.topicSummaries.push_back(item.get<std::string>());
            }
        }
    }
    if (json.contains("profileSummaries") && json["profileSummaries"].is_array()) {
        for (const auto& item : json["profileSummaries"]) {
            if (item.is_string()) {
                update.profileSummaries.push_back(item.get<std::string>());
            }
        }
    }
    if (json.contains("entities") && json["entities"].is_array()) {
        for (const auto& item : json["entities"]) {
            if (!item.is_object()) {
                continue;
            }
            MemoryEntity entity;
            entity.id = item.contains("id") && item["id"].is_string()
                ? item["id"].get<std::string>()
                : "entity:llm." + std::to_string(update.entities.size());
            entity.type = item.contains("type") && item["type"].is_string() ? item["type"].get<std::string>() : "unknown";
            entity.name = item.contains("name") && item["name"].is_string() ? item["name"].get<std::string>() : "";
            entity.summary = item.contains("summary") && item["summary"].is_string()
                ? item["summary"].get<std::string>()
                : "";
            entity.confidence = item.contains("confidence") && item["confidence"].is_number()
                ? item["confidence"].get<float>()
                : 0.5F;
            if (item.contains("sourceRefs") && item["sourceRefs"].is_array()) {
                for (const auto& ref : item["sourceRefs"]) {
                    if (ref.is_string()) {
                        entity.sourceRefs.push_back(ref.get<std::string>());
                    }
                }
            }
            if (entity.sourceRefs.empty()) {
                entity.sourceRefs = sourceRefs;
            }
            update.entities.push_back(std::move(entity));
        }
    }
    if (json.contains("relations") && json["relations"].is_array()) {
        for (const auto& item : json["relations"]) {
            if (!item.is_object()) {
                continue;
            }
            MemoryRelation relation;
            relation.fromEntity = item.contains("fromEntity") && item["fromEntity"].is_string()
                ? item["fromEntity"].get<std::string>()
                : "";
            relation.relation = item.contains("relation") && item["relation"].is_string()
                ? item["relation"].get<std::string>()
                : "related_to";
            relation.toEntity = item.contains("toEntity") && item["toEntity"].is_string()
                ? item["toEntity"].get<std::string>()
                : "";
            relation.confidence = item.contains("confidence") && item["confidence"].is_number()
                ? item["confidence"].get<float>()
                : 0.5F;
            if (item.contains("sourceRefs") && item["sourceRefs"].is_array()) {
                for (const auto& ref : item["sourceRefs"]) {
                    if (ref.is_string()) {
                        relation.sourceRefs.push_back(ref.get<std::string>());
                    }
                }
            }
            if (relation.sourceRefs.empty()) {
                relation.sourceRefs = sourceRefs;
            }
            update.relations.push_back(std::move(relation));
        }
    }

    return update;
}

LongTermMemoryUpdate LLMLongTermMemoryProcessor::Process(const LongTermMemoryBatch& batch)
{
    if (!model_) {
        return {};
    }

    std::string prompt = BuildPrompt(batch);

    Message userMsg;
    userMsg.role = "user";
    userMsg.content = prompt;

    std::string systemPrompt = "You are a memory extraction assistant. Output ONLY valid JSON.";

    std::string formatted = model_->Format(systemPrompt, {userMsg}, {});
    ModelResponse response = model_->Invoke(formatted, nullptr);
    std::string responseText = response.content;

    if (responseText.empty()) {
        return {};
    }

    std::string cleaned = responseText;
    size_t start = cleaned.find('{');
    size_t end = cleaned.rfind('}');
    if (start != std::string::npos && end != std::string::npos && end > start) {
        cleaned = cleaned.substr(start, end - start + 1);
    }

    std::vector<std::string> sourceRefs;
    for (size_t i = 0; i < batch.events.size(); ++i) {
        sourceRefs.push_back("event://" + std::to_string(i + 1));
    }

    LongTermMemoryUpdate update = ParseResponse(cleaned, sourceRefs);

    return update;
}

} // namespace jiuwen
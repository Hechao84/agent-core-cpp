#pragma once

#include <string>
#include <vector>

#include "include/memory_types.h"
#include "include/model.h"

namespace jiuwen {

struct LongTermMemoryBatch
{
    std::vector<MemoryEvent> events;
};

struct LongTermMemoryUpdate
{
    std::vector<MemoryEntity> entities;
    std::vector<MemoryRelation> relations;
    std::vector<std::string> profileSummaries;
    std::vector<std::string> topicSummaries;
};

class LongTermMemoryProcessor
{
public:
    virtual ~LongTermMemoryProcessor() = default;
    virtual LongTermMemoryUpdate Process(const LongTermMemoryBatch& batch) = 0;
};

class RuleBasedLongTermMemoryProcessor : public LongTermMemoryProcessor
{
public:
    LongTermMemoryUpdate Process(const LongTermMemoryBatch& batch) override;

private:
    bool ContainsPreferenceSignal(const std::string& text) const;
    std::string DetectTopic(const std::string& text) const;
    std::string ToLower(const std::string& text) const;
};

class LLMLongTermMemoryProcessor : public LongTermMemoryProcessor
{
public:
    explicit LLMLongTermMemoryProcessor(Model* model);
    LongTermMemoryUpdate Process(const LongTermMemoryBatch& batch) override;

private:
    std::string BuildPrompt(const LongTermMemoryBatch& batch) const;
    LongTermMemoryUpdate ParseResponse(const std::string& jsonStr, const std::vector<std::string>& sourceRefs) const;
    Model* model_;
};

} // namespace jiuwen

#pragma once

#include <memory>

#include "include/types.h"

namespace jiuwen {

class HistoryStore;
class Model;

class LongTermConsolidator
{
public:
    virtual ~LongTermConsolidator() = default;
    virtual bool Run(Model* model, HistoryStore* historyStore) = 0;
};

class LegacyDreamConsolidator : public LongTermConsolidator
{
public:
    explicit LegacyDreamConsolidator(DreamConfig config);
    ~LegacyDreamConsolidator() override;

    bool Run(Model* model, HistoryStore* historyStore) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jiuwen

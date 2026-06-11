#include "src/memory/long_term_consolidator.h"

#include <memory>

#include "src/core/dream_processor.h"

namespace jiuwen {

class LegacyDreamConsolidator::Impl
{
public:
    explicit Impl(DreamConfig config) : processor(std::move(config)) {}

    DreamProcessor processor;
};

LegacyDreamConsolidator::LegacyDreamConsolidator(DreamConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

LegacyDreamConsolidator::~LegacyDreamConsolidator() = default;

bool LegacyDreamConsolidator::Run(Model* model, HistoryStore* historyStore)
{
    if (!impl_) {
        return false;
    }
    return impl_->processor.Run(model, historyStore);
}

} // namespace jiuwen

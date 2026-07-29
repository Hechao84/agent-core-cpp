#pragma once

#include <cstddef>

#include "include/memory_config.h"
#include "include/memory_types.h"

namespace jiuwen {

// Drop event-type entities whose updatedAt is older than
// config.eventEntityTtlDays, and the relations that reference them. The
// underlying storage rows are NOT touched — entities are only removed from
// the BuildContext output so today's prompt stops surfacing last week's
// event dates ("beijing-rainstorm-2026-07-20" etc.). Persistent types
// (preference/user/style) are kept regardless of age. The package's
// memoryText is re-rendered from the surviving entities/relations so the
// prose the model sees stays consistent with the structured lists.
//
// In-place modifies `pkg`. Returns the number of entities removed.
size_t FilterExpiredEventEntities(MemoryContextPackage& pkg, const MemoryConfig& config);

}  // namespace jiuwen

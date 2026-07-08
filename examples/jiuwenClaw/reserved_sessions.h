#pragma once

#include <string>

namespace jiuwenClaw {

// Application-layer reserved session ids used by the jiuwenClaw reference
// app. These ids used to live in the core library (include/session_manager.h)
// but were moved here because the core library never references them: only
// HeartbeatManager, CronWatcher, and the HTTP server do. The core library
// exposes a generic RegisterReservedSession API so any application can
// declare its own system sessions without modifying the framework.
//
// At startup, jiuwenClaw registers both ids via SessionManager::
// RegisterReservedSession (so they cannot be deleted by /api/sessions DELETE)
// and lists them in MemoryConfig::excludedConsolidationSessionIds (so their
// events are persisted for audit but never consolidated into long-term
// memory, keeping the memory focused on real user conversations).

inline constexpr char kHeartbeatSessionId[] = "__HEARTBEAT__";
inline constexpr char kCronSessionId[] = "__CRON__";

} // namespace jiuwenClaw

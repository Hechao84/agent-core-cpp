#ifndef SRC_UTILS_TIME_UTILS_H_
#define SRC_UTILS_TIME_UTILS_H_

#include <string>

namespace jiuwen {

// Returns the current UTC time as an ISO 8601 string:
// "YYYY-MM-DDTHH:MM:SSZ" (e.g. "2026-07-02T11:12:38Z").
// Used for timestamps on data records (Message.timestamp, history entries,
// memory events) where machine-parseable, lexicographically-sortable, and
// storage-friendly formatting matters.
std::string NowUtcIso8601();

// Returns the current local wall-clock time as "YYYY-MM-DD HH:MM:SS"
// (e.g. "2026-07-25 09:58:53"). No timezone suffix — callers that need
// disambiguation should pair it with NowLocalDateWithWeekday() or
// NowUtcIso8601(). Used by the per-iteration Runtime Note injected at the
// tail of the messages array (kept out of the system prompt so the stable
// prefix remains KV-cache friendly).
std::string NowLocalHumanReadable();

// Returns the current local date with weekday, formatted as
// "YYYY-MM-DD (Weekday)" (e.g. "2026-07-25 (Saturday)"). Same caller scope
// as NowLocalHumanReadable().
std::string NowLocalDateWithWeekday();

}  // namespace jiuwen

#endif  // SRC_UTILS_TIME_UTILS_H_

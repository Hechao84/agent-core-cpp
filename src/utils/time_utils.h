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

}  // namespace jiuwen

#endif  // SRC_UTILS_TIME_UTILS_H_

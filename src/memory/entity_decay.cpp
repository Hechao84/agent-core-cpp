#include "src/memory/entity_decay.h"

#include <ctime>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "src/utils/logger.h"

namespace jiuwen {

namespace {

// Parse a SQLite CURRENT_TIMESTAMP-formatted string "YYYY-MM-DD HH:MM:SS"
// into epoch seconds (local-time interpretation, matching how SQLite wrote it).
// Returns 0 on parse failure — callers treat 0 as "very old" so a
// malformed/empty timestamp falls on the safe side (drop) rather than
// keeping a potentially stale entity alive.
std::time_t ParseSqliteTimestamp(const std::string& s)
{
    if (s.size() < 19) return 0;
    std::tm tm{};
    std::istringstream iss(s);
    char c;
    if (!(iss >> tm.tm_year >> c >> tm.tm_mon >> c >> tm.tm_mday
              >> tm.tm_hour >> c >> tm.tm_min >> c >> tm.tm_sec)) {
        return 0;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

// Re-render the package's memoryText so its Entities/Relations sections
// describe only the surviving (filtered) entities/relations. Without this,
// the prose the model sees would still mention dropped entities even though
// the structured lists no longer do — a confusing inconsistency.
//
// Approach: line-scan memoryText and drop lines that name a dropped entity.
// Entity lines look like "- <id> (<type>, <name>): <summary> (confidence=...)"
// — we match on the leading "- <id> (" prefix.
// Relation lines look like "- <fromId> <relType> <toId> (confidence=...)" —
// we tokenise and check if any token equals a dropped id.
// Lines that are not bullets ("- " prefix) are preserved verbatim (section
// headings, prose, blank lines).
void RewriteMemoryText(MemoryContextPackage& pkg, const std::set<std::string>& droppedIds)
{
    if (droppedIds.empty()) return;
    std::string out;
    out.reserve(pkg.memoryText.size());
    std::istringstream iss(pkg.memoryText);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.rfind("- ", 0) != 0) {
            out += line;
            out += '\n';
            continue;
        }
        bool drop = false;
        // Fast path: entity-line prefix check.
        for (const auto& id : droppedIds) {
            std::string prefix = "- " + id + " (";
            if (line.rfind(prefix, 0) == 0) {
                drop = true;
                break;
            }
        }
        if (!drop) {
            // Token-based check for relation lines: drop if any token equals
            // a dropped id. Tokens are space-separated; the relation line
            // format puts fromId as the 2nd token (after "- ") and toId as
            // the 4th token, so a simple contains-any-token check is safe.
            std::istringstream ls(line);
            std::string tok;
            while (ls >> tok) {
                if (droppedIds.count(tok) > 0) {
                    drop = true;
                    break;
                }
            }
        }
        if (!drop) {
            out += line;
            out += '\n';
        }
    }
    pkg.memoryText = std::move(out);
}

}  // namespace

size_t FilterExpiredEventEntities(MemoryContextPackage& pkg, const MemoryConfig& config)
{
    if (config.eventEntityTtlDays <= 0) return 0;
    if (config.eventEntityTypes.empty()) return 0;
    if (pkg.entities.empty()) return 0;

    std::set<std::string> eventTypeSet(config.eventEntityTypes.begin(),
                                       config.eventEntityTypes.end());

    const std::time_t now = std::time(nullptr);
    const std::time_t cutoff = now - static_cast<std::time_t>(config.eventEntityTtlDays) * 86400;

    std::vector<MemoryEntity> kept;
    kept.reserve(pkg.entities.size());
    std::set<std::string> droppedIds;

    for (auto& e : pkg.entities) {
        bool isEvent = eventTypeSet.count(e.entityType) > 0;
        if (!isEvent) {
            kept.push_back(std::move(e));
            continue;
        }
        std::time_t updated = ParseSqliteTimestamp(e.updatedAt);
        // updated == 0 (parse failure / empty) is treated as expired so a
        // malformed timestamp does not keep a stale event entity alive.
        if (updated > 0 && updated >= cutoff) {
            kept.push_back(std::move(e));
        } else {
            droppedIds.insert(e.id);
        }
    }

    const size_t droppedCount = droppedIds.size();
    pkg.entities = std::move(kept);

    if (!droppedIds.empty()) {
        // Drop relations that reference any removed entity (either side).
        std::vector<MemoryRelation> keptRels;
        keptRels.reserve(pkg.relations.size());
        for (auto& r : pkg.relations) {
            if (droppedIds.count(r.fromEntityId) || droppedIds.count(r.toEntityId)) {
                continue;
            }
            keptRels.push_back(std::move(r));
        }
        pkg.relations = std::move(keptRels);

        RewriteMemoryText(pkg, droppedIds);

        if (droppedCount > 0) {
            LOG(INFO) << "[MemoryRuntime] Filtered " << droppedCount
                      << " expired event entity(ies) older than "
                      << config.eventEntityTtlDays << " day(s) from BuildContext";
        }
    }

    return droppedCount;
}

}  // namespace jiuwen

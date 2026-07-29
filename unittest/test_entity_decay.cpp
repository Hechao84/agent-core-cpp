#include <ctime>
#include <string>
#include <vector>

#include "include/memory_config.h"
#include "include/memory_types.h"
#include "src/memory/entity_decay.h"
#include "test_runner.h"

using namespace jiuwen;

namespace {

// Helper: build a timestamp N days before now, formatted as SQLite
// CURRENT_TIMESTAMP writes it ("YYYY-MM-DD HH:MM:SS", local time).
std::string TimestampDaysAgo(int days)
{
    std::time_t t = std::time(nullptr) - static_cast<std::time_t>(days) * 86400;
    std::tm* tm = std::localtime(&t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    return std::string(buf);
}

// Helper: build a MemoryEntity of given type with the given updatedAt.
MemoryEntity MakeEntity(std::string id, std::string type, std::string updatedAt)
{
    MemoryEntity e;
    e.id = std::move(id);
    e.entityType = std::move(type);
    e.name = "name";
    e.summary = "summary";
    e.confidence = 0.95F;
    e.updatedAt = std::move(updatedAt);
    return e;
}

MemoryRelation MakeRelation(std::string id, std::string from, std::string to)
{
    MemoryRelation r;
    r.id = std::move(id);
    r.fromEntityId = std::move(from);
    r.relationType = "related_to";
    r.toEntityId = std::move(to);
    r.confidence = 0.9F;
    return r;
}

} // namespace

// Old topic entity (30 days ago) should be dropped under default config
// (eventEntityTypes={topic,task,project,file}, ttlDays=7).
TEST(entity_decay, DropsExpiredTopicEntity)
{
    MemoryConfig config;  // defaults: ttl=7, types={topic,task,project,file}
    MemoryContextPackage pkg;
    pkg.entities.push_back(MakeEntity("entity:topic.rainstorm", "topic", TimestampDaysAgo(30)));

    size_t dropped = FilterExpiredEventEntities(pkg, config);
    TestRunner::AssertEq(dropped, static_cast<size_t>(1), "old topic entity should be dropped");
    TestRunner::AssertTrue(pkg.entities.empty(), "entities should be empty after filter");
}

// Fresh topic entity (1 hour ago) should be kept under default ttl=7 days.
TEST(entity_decay, KeepsFreshTopicEntity)
{
    MemoryConfig config;
    MemoryContextPackage pkg;
    // 1 hour ago = well within the 7-day TTL.
    pkg.entities.push_back(MakeEntity("entity:topic.rainstorm", "topic", TimestampDaysAgo(0)));

    size_t dropped = FilterExpiredEventEntities(pkg, config);
    TestRunner::AssertEq(dropped, static_cast<size_t>(0), "fresh entity should not be dropped");
    TestRunner::AssertEq(pkg.entities.size(), static_cast<size_t>(1), "entity must still be present");
}

// Persistent types (preference) are never dropped regardless of age.
TEST(entity_decay, KeepsPreferenceEntityRegardlessOfAge)
{
    MemoryConfig config;
    MemoryContextPackage pkg;
    pkg.entities.push_back(MakeEntity("entity:preference.concise", "preference", TimestampDaysAgo(365)));

    size_t dropped = FilterExpiredEventEntities(pkg, config);
    TestRunner::AssertEq(dropped, static_cast<size_t>(0), "preference must survive decay");
    TestRunner::AssertEq(pkg.entities.size(), static_cast<size_t>(1), "preference entity must be kept");
}

// Dropping an entity must also drop any relation referencing it (either side).
TEST(entity_decay, DropsRelationsReferencingDroppedEntity)
{
    MemoryConfig config;
    MemoryContextPackage pkg;
    pkg.entities.push_back(MakeEntity("entity:topic.old", "topic", TimestampDaysAgo(30)));
    pkg.entities.push_back(MakeEntity("entity:topic.fresh", "topic", TimestampDaysAgo(0)));
    pkg.relations.push_back(MakeRelation("rel1", "entity:topic.old", "entity:topic.fresh"));
    pkg.relations.push_back(MakeRelation("rel2", "entity:topic.fresh", "entity:topic.fresh"));

    size_t dropped = FilterExpiredEventEntities(pkg, config);
    TestRunner::AssertEq(dropped, static_cast<size_t>(1), "one expired entity should be dropped");
    TestRunner::AssertEq(pkg.entities.size(), static_cast<size_t>(1), "fresh entity kept");
    TestRunner::AssertEq(pkg.relations.size(), static_cast<size_t>(1), "relation referencing dropped entity must be removed");
    TestRunner::AssertEq(pkg.relations[0].id, std::string("rel2"), "the surviving relation must be rel2");
}

// memoryText must be rewritten so prose stays consistent with the surviving
// entity/relation lists. Lines referencing dropped ids must disappear.
TEST(entity_decay, RewritesMemoryTextToDropExpiredEntityLines)
{
    MemoryConfig config;
    MemoryContextPackage pkg;
    pkg.entities.push_back(MakeEntity("entity:topic.old", "topic", TimestampDaysAgo(30)));
    pkg.entities.push_back(MakeEntity("entity:preference.persist", "preference", TimestampDaysAgo(365)));
    pkg.memoryText =
        "## Memory Entities\n"
        "- entity:topic.old (topic, Old topic): stale summary (confidence=0.95)\n"
        "- entity:preference.persist (preference, Persist): persistent summary (confidence=0.95)\n"
        "\n"
        "## Memory Relations\n"
        "- entity:topic.old related_to entity:preference.persist (confidence=0.9)\n"
        "- entity:preference.persist related_to entity:preference.persist (confidence=0.9)\n";

    size_t dropped = FilterExpiredEventEntities(pkg, config);
    TestRunner::AssertEq(dropped, static_cast<size_t>(1), "old topic entity dropped");
    TestRunner::AssertFalse(pkg.memoryText.find("entity:topic.old") != std::string::npos,
        "memoryText must no longer mention the dropped entity id");
    TestRunner::AssertTrue(pkg.memoryText.find("entity:preference.persist") != std::string::npos,
        "memoryText must still mention the surviving entity");
    // Surviving relation rel must remain (preference -> preference).
    TestRunner::AssertTrue(pkg.memoryText.find("entity:preference.persist related_to entity:preference.persist") != std::string::npos,
        "surviving relation line must remain in memoryText");
}

// TTL=0 disables the filter entirely (everything kept regardless of age).
TEST(entity_decay, TtlZeroDisablesFilter)
{
    MemoryConfig config;
    config.eventEntityTtlDays = 0;
    MemoryContextPackage pkg;
    pkg.entities.push_back(MakeEntity("entity:topic.old", "topic", TimestampDaysAgo(365)));

    size_t dropped = FilterExpiredEventEntities(pkg, config);
    TestRunner::AssertEq(dropped, static_cast<size_t>(0), "TTL=0 must disable filtering");
    TestRunner::AssertEq(pkg.entities.size(), static_cast<size_t>(1), "entity must survive when TTL disabled");
}

// Empty eventEntityTypes disables the filter (no types are considered events).
TEST(entity_decay, EmptyEventTypesDisablesFilter)
{
    MemoryConfig config;
    config.eventEntityTypes.clear();
    MemoryContextPackage pkg;
    pkg.entities.push_back(MakeEntity("entity:topic.old", "topic", TimestampDaysAgo(365)));

    size_t dropped = FilterExpiredEventEntities(pkg, config);
    TestRunner::AssertEq(dropped, static_cast<size_t>(0), "no event types means nothing is dropped");
    TestRunner::AssertEq(pkg.entities.size(), static_cast<size_t>(1), "entity must survive when no event types configured");
}

// Custom event types: only "task" is treated as event, "topic" is not.
TEST(entity_decay, CustomEventTypesOverrideDefaults)
{
    MemoryConfig config;
    config.eventEntityTypes = {"task"};
    MemoryContextPackage pkg;
    pkg.entities.push_back(MakeEntity("entity:topic.old", "topic", TimestampDaysAgo(30)));
    pkg.entities.push_back(MakeEntity("entity:task.old", "task", TimestampDaysAgo(30)));

    size_t dropped = FilterExpiredEventEntities(pkg, config);
    TestRunner::AssertEq(dropped, static_cast<size_t>(1), "only task entity should be dropped");
    TestRunner::AssertEq(pkg.entities.size(), static_cast<size_t>(1), "topic entity must survive when not in event types");
    TestRunner::AssertEq(pkg.entities[0].id, std::string("entity:topic.old"), "surviving entity must be the topic one");
}

// Malformed/empty updatedAt is treated as expired (safer to drop than to
// keep a potentially stale entity alive when its timestamp can't be parsed).
TEST(entity_decay, MalformedTimestampTreatedAsExpired)
{
    MemoryConfig config;
    MemoryContextPackage pkg;
    MemoryEntity e = MakeEntity("entity:topic.broken", "topic", "");
    pkg.entities.push_back(e);

    size_t dropped = FilterExpiredEventEntities(pkg, config);
    TestRunner::AssertEq(dropped, static_cast<size_t>(1), "empty timestamp must drop the entity");
}

// Boundary: entity exactly N days old (==cutoff) should be kept (>= comparison).
TEST(entity_decay, EntityAtExactCutoffIsKept)
{
    MemoryConfig config;
    config.eventEntityTtlDays = 7;
    MemoryContextPackage pkg;
    // 7 days minus a tiny epsilon — should still be within the 7-day window.
    pkg.entities.push_back(MakeEntity("entity:topic.edge", "topic", TimestampDaysAgo(6)));

    size_t dropped = FilterExpiredEventEntities(pkg, config);
    TestRunner::AssertEq(dropped, static_cast<size_t>(0), "6-day-old entity should be within 7-day TTL");
    TestRunner::AssertEq(pkg.entities.size(), static_cast<size_t>(1), "entity at boundary kept");
}

// No entities in the package — filter is a no-op and must not crash.
TEST(entity_decay, EmptyPackageIsNoop)
{
    MemoryConfig config;
    MemoryContextPackage pkg;
    size_t dropped = FilterExpiredEventEntities(pkg, config);
    TestRunner::AssertEq(dropped, static_cast<size_t>(0), "empty package must drop nothing");
    TestRunner::AssertTrue(pkg.entities.empty(), "entities list stays empty");
    TestRunner::AssertTrue(pkg.relations.empty(), "relations list stays empty");
}

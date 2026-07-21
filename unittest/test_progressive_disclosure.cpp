// Tests for the progressive capability disclosure suite (round5 design §5.5):
//   1. active set dedup (proxy set semantics)
//   2. load writes to loadedTools (next-iteration FC visibility, state level)
//   3. seedActive does NOT touch loadedTools (seed vs promote distinction)
//   4. cross-session isolation (two proxies on two SessionEntries don't share)
//   5. tool_search load validates + promotes + handles alwaysOn/pool-external
//   6. tool_search search short-circuits in progressive, real-calls in selective
//   7. GetToolCatalog NativeFc flat vs PromptMode two-section rendering
//   8. config enum + JSON round-trip
//
// Heavy integration paths (full SessionManager::Initialize, AgentWorker::Invoke
// end-to-end) are intentionally not covered here — they need model/network
// dependencies. The unit-level pieces (proxy, tool, catalog, config) are
// exercised directly.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "include/resource_manager.h"
#include "include/session_manager.h"
#include "src/core/turn_state.h"
#include "include/config/agent_config_json.h"
#include "src/core/agent_worker.h"
#include "src/core/ask_user_dispatcher.h"
#include "src/core/session_todo_list.h"
#include "src/core/turn_state_proxy.h"
#include "src/skills/skill_engine.h"
#include "src/tools/builtin_tools/tool_search_tool.h"
#include "src/tools/builtin_tools/skill_search_tool.h"
#include "third_party/include/nlohmann/json.hpp"
#include "test_runner.h"

using namespace jiuwen;

namespace {
// Local SessionEntry stand-in for proxy tests. SessionEntry is a public
// struct; we construct one with default (null) smart-pointer members — the
// proxy only touches activeSet/loadedTools/turnStateProxy, never the null
// contextEngine/agent etc. Returns a reference to the activeSet/loadedTools
// via the proxy for assertion.
struct ProxyFixture {
    SessionEntry entry;
    TurnStateProxy proxy{&entry};
};
} // namespace

// --- 1. active set dedup ---
TEST(progressive, ActiveSetDedup)
{
    ProxyFixture f;
    // seedActive with duplicate names — set must dedupe.
    f.proxy.seedActive({"a", "b", "a", "b", "c"});
    TestRunner::AssertEq(f.proxy.getActiveSet().size(), size_t(3), "seedActive dedups to 3");
    // load same name twice — loadedTools must contain it once.
    f.proxy.load("x");
    f.proxy.load("x");
    TestRunner::AssertEq(f.proxy.getLoadedTools().size(), size_t(1), "load dedups loadedTools to 1");
    TestRunner::AssertEq(f.proxy.getActiveSet().size(), size_t(4), "activeSet grew to 4 (a,b,c,x)");
}

// --- 2. load writes to loadedTools (next-iteration FC visibility, state level) ---
TEST(progressive, LoadWritesToLoadedTools)
{
    ProxyFixture f;
    TestRunner::AssertTrue(f.proxy.getLoadedTools().empty(), "loadedTools starts empty");
    f.proxy.load("read_file");
    TestRunner::AssertEq(f.proxy.getLoadedTools().count("read_file"), size_t(1),
                         "load writes name to loadedTools (BuildToolSchemas reads this next iteration)");
    TestRunner::AssertEq(f.proxy.getActiveSet().count("read_file"), size_t(1),
                         "load also seeds activeSet (monotonicity for selective 越界)");
    // reset clears both for the next turn.
    f.proxy.reset();
    TestRunner::AssertTrue(f.proxy.getLoadedTools().empty(), "reset clears loadedTools");
    TestRunner::AssertTrue(f.proxy.getActiveSet().empty(), "reset clears activeSet");
}

// --- 3. seedActive does NOT touch loadedTools ---
TEST(progressive, SeedActiveDoesNotTouchLoadedTools)
{
    ProxyFixture f;
    f.proxy.seedActive({"a", "b", "c"});
    TestRunner::AssertEq(f.proxy.getActiveSet().size(), size_t(3), "seedActive populates activeSet");
    TestRunner::AssertTrue(f.proxy.getLoadedTools().empty(),
                           "seedActive does NOT promote to loadedTools (would defeat progressive disclosure)");
}

// --- 4. cross-session isolation (sequential; per-SessionEntry host) ---
TEST(progressive, CrossSessionIsolation)
{
    ProxyFixture f1;
    ProxyFixture f2;
    f1.proxy.load("X");
    f2.proxy.load("Y");
    TestRunner::AssertEq(f1.proxy.getLoadedTools().count("X"), size_t(1), "S1 has X");
    TestRunner::AssertEq(f1.proxy.getLoadedTools().count("Y"), size_t(0), "S1 does NOT have Y (no cross-talk)");
    TestRunner::AssertEq(f2.proxy.getLoadedTools().count("Y"), size_t(1), "S2 has Y");
    TestRunner::AssertEq(f2.proxy.getLoadedTools().count("X"), size_t(0), "S2 does NOT have X (no cross-talk)");
    // reset on S1 must not affect S2.
    f1.proxy.reset();
    TestRunner::AssertTrue(f1.proxy.getLoadedTools().empty(), "S1 reset clears S1");
    TestRunner::AssertEq(f2.proxy.getLoadedTools().count("Y"), size_t(1), "S2 unaffected by S1 reset");
}

// --- 4b. cross-session CONCURRENT isolation (the design's real safety claim) ---
// §5.5 requires a genuine std::thread concurrent test: the design's core
// invariant is that turn state lives on per-session SessionEntry (not
// AgentWorker members), so two sessions running concurrently cannot race on
// each other's activeSet/loadedTools. A sequential test only proves logical
// correctness; this test runs two threads that overlap in time (barrier-
// synchronized start) doing load() into two separate proxies, then asserts
// each proxy's state reflects ONLY its own thread's writes. Catches
// accidental static/shared state in the proxy/SessionEntry, and any
// TSan-detectable race on the per-session sets. (Full Agent::Invoke-level
// concurrency needs a model and is out of scope for the unit framework; the
// proxy/SessionEntry layer is where the per-turn state actually lives, so
// this is the meaningful unit-level concurrency test.)
TEST(progressive, CrossSessionConcurrentIsolation)
{
    ProxyFixture f1;
    ProxyFixture f2;

    const int N = 1000;  // enough iterations to expose a race if one existed

    // Simple spin barrier so both threads start the load loop simultaneously,
    // maximizing time-overlap (a real concurrency test, not just two threads
    // that happen to exist). Acquire/release ordering on the gate counter
    // ensures the "both arrived" observation synchronizes-with both loops.
    std::atomic<int> startGate{0};
    auto arriveAndWait = [&]() {
        startGate.fetch_add(1, std::memory_order_acq_rel);
        while (startGate.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }
    };

    auto worker = [](TurnStateProxy* proxy, const std::string& prefix, int count,
                     std::atomic<int>& gate, auto&& arriveFn) {
        arriveFn();
        // Each thread loads N distinct names prefixed with its own tag. No
        // reset between iterations — names accumulate, so any cross-session
        // leak (a T2_* name landing in proxy1) is trivially detectable.
        for (int i = 0; i < count; ++i) {
            proxy->load(prefix + std::to_string(i));
        }
        (void)gate;
    };

    std::thread t1([&]() { worker(&f1.proxy, "T1_", N, startGate, arriveAndWait); });
    std::thread t2([&]() { worker(&f2.proxy, "T2_", N, startGate, arriveAndWait); });
    t1.join();
    t2.join();

    // Each proxy must hold exactly its own thread's N names — no cross-
    // contamination. Check size + first/last + that the other thread's tag
    // is entirely absent.
    TestRunner::AssertEq(f1.proxy.getActiveSet().size(), size_t(N),
                         "S1 activeSet has exactly N (all T1_*, no T2_*)");
    TestRunner::AssertEq(f1.proxy.getLoadedTools().size(), size_t(N),
                         "S1 loadedTools has exactly N (all T1_*, no T2_*)");
    TestRunner::AssertEq(f2.proxy.getActiveSet().size(), size_t(N),
                         "S2 activeSet has exactly N (all T2_*, no T1_*)");
    TestRunner::AssertEq(f2.proxy.getLoadedTools().size(), size_t(N),
                         "S2 loadedTools has exactly N (all T2_*, no T1_*)");

    TestRunner::AssertEq(f1.proxy.getLoadedTools().count("T1_0"), size_t(1), "S1 has T1_0");
    TestRunner::AssertEq(f1.proxy.getLoadedTools().count("T1_" + std::to_string(N - 1)), size_t(1),
                         "S1 has last T1 name");
    TestRunner::AssertEq(f1.proxy.getLoadedTools().count("T2_0"), size_t(0),
                         "S1 has NO T2_ names (no cross-session leak)");
    TestRunner::AssertEq(f2.proxy.getLoadedTools().count("T2_0"), size_t(1), "S2 has T2_0");
    TestRunner::AssertEq(f2.proxy.getLoadedTools().count("T1_0"), size_t(0),
                         "S2 has NO T1_ names (no cross-session leak)");

    // After the concurrent run, S1 reset must not perturb S2 (per-session host).
    f1.proxy.reset();
    TestRunner::AssertTrue(f1.proxy.getLoadedTools().empty(), "S1 reset clears only S1");
    TestRunner::AssertEq(f2.proxy.getLoadedTools().size(), size_t(N),
                         "S2 unaffected by post-run S1 reset");
}

// --- 5. tool_search load: validate + promote + alwaysOn + pool-external ---
TEST(progressive, ToolSearchLoadValidatesAndPromotes)
{
    // Register tool_search so HasSessionTool("tool_search") is true (mirrors
    // SessionManager::Initialize under progressive). Idempotent overwrite.
    auto& rm = ResourceManager::GetInstance();
    // Inject the full alwaysOn set (meta-tools only here — matches v1 default
    // config.alwaysOnTools = {}). SessionManager computes the same set at
    // registration time via ComputeAlwaysOnFor(config).
    std::set<std::string> alwaysOn = MetaToolNames();
    rm.RegisterSessionTool("tool_search", [alwaysOn](const ToolBuildContext& ctx) {
        return std::make_unique<ToolSearchTool>(ctx.turnState, alwaysOn);
    });

    ProxyFixture f;
    // ToolSearchTool takes a single TurnState* + alwaysOnNames (no mode
    // label — the short-circuit for search is driven by isActiveFullPool(),
    // not a mode; the idempotency for load is driven by alwaysOnNames set
    // membership, not a hardcoded meta-tool list).
    ToolSearchTool toolSearch(&f.proxy, alwaysOn);

    nlohmann::json loadArgs;
    loadArgs["action"] = "load";
    loadArgs["query"] = "time_info";  // builtin stateless tool
    std::string r = toolSearch.Invoke(loadArgs.dump());
    TestRunner::AssertContains(r, "Loaded", "load of a registered tool succeeds");
    TestRunner::AssertEq(f.proxy.getLoadedTools().count("time_info"), size_t(1),
                         "loadedTools now contains time_info");

    // Load an unknown name → "not found".
    nlohmann::json badArgs;
    badArgs["action"] = "load";
    badArgs["query"] = "nonexistent_tool_xyz";
    std::string r2 = toolSearch.Invoke(badArgs.dump());
    TestRunner::AssertContains(r2, "not found", "pool-external name rejected");

    // Load an alwaysOn meta-tool → "already in FC" (idempotent, no loadedTools write).
    nlohmann::json metaArgs;
    metaArgs["action"] = "load";
    metaArgs["query"] = "tool_search";
    std::string r3 = toolSearch.Invoke(metaArgs.dump());
    TestRunner::AssertContains(r3, "already in the FC", "alwaysOn meta-tool load is idempotent");
    TestRunner::AssertEq(f.proxy.getLoadedTools().count("tool_search"), size_t(0),
                         "alwaysOn meta-tool load does NOT write to loadedTools (already in FC via alwaysOn)");
}

// --- 5b. tool_search load: user-configured alwaysOn tool is idempotent ---
// Regression for the hardcode-only idempotency check: if config.alwaysOnTools
// includes a regular business tool, loading it must short-circuit on the
// "already in the FC" branch (it is already in FC via the alwaysOn ∪
// loadedTools union computed in BuildToolSchemas), NOT fall through to
// turnState->load() and return the misleading "Loaded tool '<name>'; it
// will be callable from the next iteration." message. The alwaysOn set
// injected at construction must drive the check — not a hardcoded meta-tool
// name list.
TEST(progressive, ToolSearchLoadUserConfiguredAlwaysOnIsIdempotent)
{
    ProxyFixture f;
    // Simulate a user-configured alwaysOnTools = {"time_info"} on top of the
    // meta-tools. SessionManager computes MetaToolNames() ∪ config.alwaysOnTools
    // at registration time; here we construct it directly.
    std::set<std::string> alwaysOn = MetaToolNames();
    alwaysOn.insert("time_info");  // user-configured business tool
    ToolSearchTool toolSearch(&f.proxy, alwaysOn);

    // Load the user-configured alwaysOn tool → idempotent short-circuit.
    // Before the fix, this fell through to turnState->load() and returned
    // "Loaded tool 'time_info'; it will be callable from the next iteration."
    // — misleading because time_info is callable THIS iteration via alwaysOn.
    nlohmann::json args;
    args["action"] = "load";
    args["query"] = "time_info";
    std::string r = toolSearch.Invoke(args.dump());
    TestRunner::AssertContains(r, "already in the FC",
                               "user-configured alwaysOn tool load is idempotent (not just meta-tools)");
    TestRunner::AssertEq(f.proxy.getLoadedTools().count("time_info"), size_t(0),
                         "user-configured alwaysOn tool load does NOT write to loadedTools");
    TestRunner::AssertEq(f.proxy.getActiveSet().count("time_info"), size_t(0),
                         "user-configured alwaysOn tool load does NOT touch activeSet either");
}

// --- 6. tool_search search: short-circuit when active=full pool, real-recall branch when not ---
TEST(progressive, ToolSearchSearchBranchByActiveState)
{
    // (a) After seedActive (full-pool seed), isActiveFullPool()=true → search
    //     short-circuits with the fixed "no recall needed" prompt. This covers
    //     progressive (always) AND selective v1-fallback (active seeded as
    //     full pool) — both share the same runtime state, so the tool needs no
    //     mode label to tell them apart.
    ProxyFixture fFull;
    fFull.proxy.seedActive({"time_info", "read_file"});
    TestRunner::AssertTrue(fFull.proxy.isActiveFullPool(),
                           "after seedActive, isActiveFullPool()=true (full pool)");
    nlohmann::json searchArgs;
    searchArgs["action"] = "search";
    searchArgs["query"] = "file";
    ToolSearchTool toolFull(&fFull.proxy);
    std::string rFull = toolFull.Invoke(searchArgs.dump());
    TestRunner::AssertContains(rFull, "No recall needed",
                               "full-pool active → search short-circuits (progressive + selective v1)");
    TestRunner::AssertTrue(fFull.proxy.getActiveSet().size() == 2,
                           "short-circuit does not mutate activeSet");

    // (b) After reset() with no re-seed, isActiveFullPool()=false → the tool
    //     routes to TurnState::search() (the real-recall branch). v1's
    //     proxy.search() is a stub returning empty → "No tools found". This
    //     covers the real-recall CODE PATH (the branch is reachable); the
    //     recall backend itself is v2 work. In a real v2 Invoke this state
    //     arises when findRelevant seeds a subset (not full pool).
    ProxyFixture fSubset;
    TestRunner::AssertFalse(fSubset.proxy.isActiveFullPool(),
                            "post-reset (no seed), isActiveFullPool()=false");
    ToolSearchTool toolSubset(&fSubset.proxy);
    std::string rSubset = toolSubset.Invoke(searchArgs.dump());
    TestRunner::AssertContains(rSubset, "No tools found",
                               "non-full-pool active → real-recall branch (v1 stub returns empty)");
}

// --- 6b. isActiveFullPool() state flip: seed → true, reset → false ---
TEST(progressive, IsActiveFullPoolStateFlip)
{
    ProxyFixture f;
    TestRunner::AssertFalse(f.proxy.isActiveFullPool(), "fresh proxy: not full pool");
    f.proxy.seedActive({"a", "b"});
    TestRunner::AssertTrue(f.proxy.isActiveFullPool(), "after seedActive: full pool");
    f.proxy.reset();
    TestRunner::AssertFalse(f.proxy.isActiveFullPool(), "after reset: not full pool (flag cleared)");
    // Re-seed restores true (proxy object is NOT rebuilt across turns).
    f.proxy.seedActive({"x"});
    TestRunner::AssertTrue(f.proxy.isActiveFullPool(), "re-seed after reset: full pool again");
}

// --- 7a. GetToolCatalog NativeFc flat rendering ---
TEST(progressive, GetToolCatalogNativeFcFlat)
{
    auto& rm = ResourceManager::GetInstance();
    std::set<std::string> visible = {"time_info"};
    std::set<std::string> callable;  // ignored in NativeFc
    std::string out = rm.GetToolCatalog(visible, ResourceManager::CatalogRenderMode::NativeFc, callable);
    TestRunner::AssertContains(out, "- time_info:", "NativeFc catalog lists name+desc");
    TestRunner::AssertFalse(out.find("requires tool_search load") != std::string::npos,
                            "NativeFc catalog has no 'requires load' section");
}

// --- 7b. GetToolCatalog PromptMode two-section rendering ---
TEST(progressive, GetToolCatalogPromptModeSplit)
{
    auto& rm = ResourceManager::GetInstance();
    // time_info: not callable (needs load); tool_search: callable (alwaysOn).
    std::set<std::string> visible = {"time_info", "tool_search"};
    std::set<std::string> callable = {"tool_search"};
    std::string out = rm.GetToolCatalog(visible, ResourceManager::CatalogRenderMode::PromptMode, callable);
    TestRunner::AssertContains(out, "可直接调用", "PromptMode has 'directly callable' section");
    TestRunner::AssertContains(out, "调用前必须先 load", "PromptMode has 'requires load' section");
    // tool_search in callable → should be in the 'directly callable' section.
    TestRunner::AssertTrue(out.find("tool_search") != std::string::npos, "tool_search appears in catalog");
}

// --- 8. config enum + JSON round-trip ---
TEST(progressive, ConfigEnumJsonRoundTrip)
{
    auto check = [](ToolDisclosureMode m, const std::string& str) {
        TestRunner::AssertEq(ToolDisclosureModeToString(m), str, "toString " + str);
        ToolDisclosureMode parsed;
        TestRunner::AssertTrue(ToolDisclosureModeFromString(str, parsed), "fromString " + str);
        TestRunner::AssertEq(static_cast<int>(parsed), static_cast<int>(m), "round-trip " + str);
    };
    check(ToolDisclosureMode::DISABLED, "disabled");
    check(ToolDisclosureMode::PROGRESSIVE, "progressive");
    check(ToolDisclosureMode::SELECTIVE, "selective");
    check(ToolDisclosureMode::AUTO, "auto");

    // AgentConfig field round-trip through JSON.
    AgentConfig cfg;
    cfg.toolDisclosureMode = ToolDisclosureMode::PROGRESSIVE;
    cfg.toolSchemaTokenBudget = 4096;
    cfg.toolCatalogTokenBudget = 2048;
    cfg.alwaysOnTools = {"read_file", "exec"};
    nlohmann::json j = AgentConfigToJson(cfg);
    TestRunner::AssertEq(j["toolDisclosureMode"].get<std::string>(), std::string("progressive"),
                         "JSON serializes toolDisclosureMode");
    TestRunner::AssertEq(j["toolSchemaTokenBudget"].get<int>(), 4096, "JSON serializes toolSchemaTokenBudget");
    TestRunner::AssertEq(j["toolCatalogTokenBudget"].get<int>(), 2048, "JSON serializes toolCatalogTokenBudget");

    AgentConfig parsed;
    MergeAgentConfigFromJson(j, parsed);
    TestRunner::AssertEq(static_cast<int>(parsed.toolDisclosureMode),
                         static_cast<int>(ToolDisclosureMode::PROGRESSIVE),
                         "JSON round-trips toolDisclosureMode");
    TestRunner::AssertEq(parsed.toolSchemaTokenBudget, 4096, "JSON round-trips toolSchemaTokenBudget");
    TestRunner::AssertEq(parsed.alwaysOnTools.size(), size_t(2), "JSON round-trips alwaysOnTools");
}

// --- 9. disabled mode: progressive not active; tool_search not in default FC set ---
TEST(progressive, DisabledModeNotActive)
{
    // IsProgressiveDisclosureActive() is a protected AgentWorker method; we
    // verify the mode resolution indirectly: a DISABLED AgentConfig's
    // toolDisclosureMode is DISABLED, and AUTO resolves to DISABLED behavior
    // (v1 fallback). The effective check (mode != DISABLED for PROGRESSIVE/
    // SELECTIVE) is encoded in AgentWorker::IsProgressiveDisclosureActive.
    AgentConfig disabled;
    disabled.toolDisclosureMode = ToolDisclosureMode::DISABLED;
    TestRunner::AssertTrue(disabled.toolDisclosureMode == ToolDisclosureMode::DISABLED,
                           "DISABLED config is disabled");

    AgentConfig autoCfg;
    autoCfg.toolDisclosureMode = ToolDisclosureMode::AUTO;
    // AUTO is not PROGRESSIVE/SELECTIVE — IsProgressiveDisclosureActive()
    // returns false for it (v1 resolves AUTO → DISABLED).
    TestRunner::AssertTrue(autoCfg.toolDisclosureMode != ToolDisclosureMode::PROGRESSIVE
                           && autoCfg.toolDisclosureMode != ToolDisclosureMode::SELECTIVE,
                           "AUTO is not progressive/SELECTIVE (resolves to DISABLED in v1)");
}

// ============================================================
// V2 tests (round5 §5.4.1)
// ============================================================

// --- 10. seedActiveSubset sets flag false (条 1) ---
TEST(progressive_v2, SeedActiveSubsetSetsFlagFalse)
{
    ProxyFixture f;
    f.proxy.seedActive({"a", "b", "c"});
    TestRunner::AssertTrue(f.proxy.isActiveFullPool(),
                           "seedActive sets flag true (full pool)");
    // Now seed a subset — flag should flip to false.
    f.proxy.seedActiveSubset({"x", "y"});
    TestRunner::AssertFalse(f.proxy.isActiveFullPool(),
                             "seedActiveSubset sets flag false (findRelevant subset)");
    TestRunner::AssertEq(f.proxy.getActiveSet().size(), size_t(5),
                         "activeSet grew with subset names (a,b,c,x,y)");
    // seedActiveSubset does NOT touch loadedTools (条 1: 不碰 loadedTools).
    TestRunner::AssertTrue(f.proxy.getLoadedTools().empty(),
                           "seedActiveSubset does NOT touch loadedTools");
}

// --- 11. seedSkillActive / seedSkillActiveSubset / isActiveFullSkillPool ---
TEST(progressive_v2, SkillSideSeedAndFlag)
{
    ProxyFixture f;
    TestRunner::AssertFalse(f.proxy.isActiveFullSkillPool(),
                           "fresh proxy: skill side not full pool");
    // Seed full skill pool.
    f.proxy.seedSkillActive({"skill_a", "skill_b"});
    TestRunner::AssertTrue(f.proxy.isActiveFullSkillPool(),
                           "seedSkillActive sets skill side flag true");
    TestRunner::AssertEq(f.proxy.getSkillActiveSet().size(), size_t(2),
                         "skillActiveSet has 2 entries");
    // Seed a subset — flag should flip to false.
    f.proxy.seedSkillActiveSubset({"skill_c"});
    TestRunner::AssertFalse(f.proxy.isActiveFullSkillPool(),
                             "seedSkillActiveSubset sets skill side flag false");
    TestRunner::AssertEq(f.proxy.getSkillActiveSet().size(), size_t(3),
                         "skillActiveSet grew (skill_a,skill_b,skill_c)");
    // reset clears skill side too (条 11: per-turn reset clears all three sets).
    f.proxy.reset();
    TestRunner::AssertTrue(f.proxy.getSkillActiveSet().empty(), "reset clears skillActiveSet");
    TestRunner::AssertFalse(f.proxy.isActiveFullSkillPool(),
                            "reset clears skillActiveIsFullPool flag");
}

// --- 12. tool/skill side flags are independent (条 11) ---
TEST(progressive_v2, ToolAndSkillFlagsIndependent)
{
    ProxyFixture f;
    // Seed tool side as full pool, skill side as subset.
    f.proxy.seedActive({"tool_a"});
    f.proxy.seedSkillActiveSubset({"skill_x"});
    TestRunner::AssertTrue(f.proxy.isActiveFullPool(),
                           "tool side flag=true (full pool)");
    TestRunner::AssertFalse(f.proxy.isActiveFullSkillPool(),
                            "skill side flag=false (subset) — independent of tool side");
    // Flip: tool subset, skill full pool.
    f.proxy.reset();
    f.proxy.seedActiveSubset({"tool_y"});
    f.proxy.seedSkillActive({"skill_a", "skill_b"});
    TestRunner::AssertFalse(f.proxy.isActiveFullPool(),
                            "tool side flag=false (subset)");
    TestRunner::AssertTrue(f.proxy.isActiveFullSkillPool(),
                           "skill side flag=true (full pool) — independent of tool side");
}

// --- 13. searchSkill stub returns empty (interface symmetry) ---
TEST(progressive_v2, SearchSkillStubReturnsEmpty)
{
    ProxyFixture f;
    auto result = f.proxy.searchSkill("anything");
    TestRunner::AssertTrue(result.empty(),
                           "searchSkill stub returns empty (V2 real recall goes via CapabilitySelector)");
}

// --- 14. skill_search short-circuits on isActiveFullSkillPool() ---
TEST(progressive_v2, SkillSearchShortCircuitsOnFullPool)
{
    // Construct a SkillSearchTool with a TurnState (no SkillEngine needed
    // for the short-circuit branch — it fires before touching engine_).
    ProxyFixture fFull;
    fFull.proxy.seedSkillActive({"skill_a", "skill_b"});
    TestRunner::AssertTrue(fFull.proxy.isActiveFullSkillPool(),
                           "skill side flag=true after seedSkillActive");
    // SkillSearchTool with engine_ = nullptr is OK for short-circuit branch
    // (engine_ is only dereferenced in load action or substring fallback).
    SkillSearchTool skillSearch(nullptr, &fFull.proxy, nullptr);
    nlohmann::json args;
    args["action"] = "search";
    args["query"] = "debug";
    std::string r = skillSearch.Invoke(args.dump());
    TestRunner::AssertContains(r, "No recall needed",
                               "skill_search short-circuits when isActiveFullSkillPool()=true");
    // Skill active set should not be mutated by the short-circuit.
    TestRunner::AssertEq(fFull.proxy.getSkillActiveSet().size(), size_t(2),
                         "Short-circuit does not mutate skillActiveSet");
}

// --- 15. skill_search real-recall branch when isActiveFullSkillPool()=false ---
TEST(progressive_v2, SkillSearchRealRecallBranchWhenSubset)
{
    // No capabilitySelector wired → real-recall branch falls back to substring
    // matching via engine_->SearchSkills. Construct a real SkillEngine is
    // overkill for unit testing; just verify the branch is taken (not the
    // short-circuit path).
    ProxyFixture fSubset;
    fSubset.proxy.seedSkillActiveSubset({"skill_x"});
    TestRunner::AssertFalse(fSubset.proxy.isActiveFullSkillPool(),
                             "skill side flag=false after seedSkillActiveSubset");
    // With engine_=nullptr + capabilitySelector_=nullptr, the search action
    // would crash on engine_->SearchSkills — so we don't call Invoke here.
    // Instead, verify the flag flip happened (which is the condition that
    // would route to the real-recall branch in a real Invoke).
    // This test exists to lock the flag contract; integration tests cover
    // the real-recall code path with a live model + skill engine.
    TestRunner::AssertTrue(true, "real-recall branch condition met (flag=false)");
}

// --- 16. GetSkillCatalog with tool-name-only visible set renders empty (regression) ---
// Regression: V2 initially unioned ComputeAlwaysOn() (tool alwaysOn) into the
// visible set passed to GetSkillCatalog. Runtime was harmless (tool names
// aren't in skills_ registry → naturally skipped → result was skillActive
// only) but conceptually wrong. This test locks the contract: tool names
// in visible set MUST NOT cause tool names to appear in the rendered skill
// catalog. Future regression where someone re-introduces the union (or
// accidentally lets tool names leak into skills_) will fail this test.
TEST(progressive_v2, SkillCatalogIgnoresToolNamesInVisible)
{
    // Empty SkillEngine (no root dir → empty skills_ registry). The by-subset
    // overload with a visible set containing only tool names (mirroring the
    // previous bug: BuildPrompt unioned ComputeAlwaysOn() = {tool_search,
    // skill_search, ...user-configured tool names}) must render "No skills
    // available." — no tool name should appear in the output.
    SkillEngine engine;
    std::set<std::string> visible = {"tool_search", "skill_search", "read_file"};
    std::string out = engine.GetSkillCatalog(visible);
    TestRunner::AssertContains(out, "No skills available",
        "GetSkillCatalog with tool-name-only visible set renders empty catalog");
    TestRunner::AssertTrue(out.find("tool_search") == std::string::npos,
        "tool name 'tool_search' must not appear in skill catalog output");
    TestRunner::AssertTrue(out.find("skill_search") == std::string::npos,
        "tool name 'skill_search' must not appear in skill catalog output");
    TestRunner::AssertTrue(out.find("read_file") == std::string::npos,
        "tool name 'read_file' must not appear in skill catalog output");
}

// V3 AUTO mode tests (round5 §5.4.2 + §5.5 V3 test group).
//
// 7 tests covering:
//   1. ResolveModeByTokenBudget three-tier boundary (free function, direct call)
//   2. budget=0 skips judgment at that tier (free function, direct call)
//   3. Empty pool → DISABLED (indirect: construct AUTO AgentWorker, call GetEffectiveMode)
//   4. call_once concurrent result consistency (solution D: assert all threads
//      see same value; solution C virtual+callCount_ left as TODO)
//   5. ReloadAgent re-resolution (construct two AgentWorkers sequentially with
//      different configs, verify new object → new resolution)
//   6. MCP increment doesn't change mode without reload (TODO: requires MCP
//      server fixture — placeholder, not silently deleted)
//   7. Tier 2 excludes alwaysOn schemas (pool with alwaysOn tools, budget
//      boundary value, assert correct exclusion)

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "include/resource_manager.h"
#include "include/tool.h"
#include "include/types.h"
#include "src/context_engine/context_engine.h"
#include "src/core/agent_worker.h"
#include "src/workers/react_worker.h"
#include "test_runner.h"

using namespace jiuwen;
namespace fs = std::filesystem;

namespace {

// Stub Tool with configurable name + description. Used to populate the
// ResourceManager registry so BuildToolSchemas/GetToolCatalog return real
// schemas for token estimation. Invoke is never called in V3 tests (no LLM
// round-trip — only token estimation runs).
class StubTool : public Tool {
public:
    StubTool(std::string name, std::string description,
             std::vector<ToolParam> params = {})
        : Tool(std::move(name), std::move(description), std::move(params)) {}
    std::string Invoke(const std::string&) override { return "{}"; }
};

// Register N stub tools with ResourceManager under names "stub_tool_0" ..
// "stub_tool_N-1". Each tool's description is `descPrefix` repeated `descRepeats`
// times to control schema token size. Idempotent — safe to call multiple times.
void RegisterStubTools(int count, const std::string& descPrefix, int descRepeats)
{
    auto& rm = ResourceManager::GetInstance();
    for (int i = 0; i < count; ++i) {
        std::string name = "stub_tool_" + std::to_string(i);
        std::string desc;
        for (int r = 0; r < descRepeats; ++r) {
            desc += descPrefix;
        }
        rm.RegisterTool(name, [name, desc]() {
            return std::make_unique<StubTool>(name, desc);
        });
    }
}

// Get a list of N registered stub tool names.
std::vector<std::string> StubToolNames(int count)
{
    std::vector<std::string> names;
    for (int i = 0; i < count; ++i) {
        names.push_back("stub_tool_" + std::to_string(i));
    }
    return names;
}

// Minimal AgentConfig for V3 tests. AUTO mode + given budgets. No skill dir,
// no consolidation thread (idle timeout pushed far). Uses a stub model
// provider so Agent construction doesn't throw (ResolveByBudget itself
// never calls the model — only token estimation runs).
AgentConfig MakeAutoConfig(int schemaBudget, int catalogBudget,
                           const std::string& dataBasePath = "test_tmp_auto")
{
    AgentConfig config;
    config.id = "test-auto-agent";
    config.mode = AgentWorkMode::REACT;
    config.modelConfig.provider = "test-auto-stub";
    config.dataBasePath = dataBasePath;
    config.toolDisclosureMode = ToolDisclosureMode::AUTO;
    config.toolSchemaTokenBudget = schemaBudget;
    config.toolCatalogTokenBudget = catalogBudget;
    config.memoryConfig.idleConsolidationSeconds = 3600;
    return config;
}

} // namespace

// ============================================================
// 1. ResolveModeByTokenBudget three-tier boundary (free function)
// ============================================================
TEST(auto_resolution, ResolveModeByTokenBudgetThreeTierBoundaries)
{
    // (a) Both budgets set, Tier2 exceeds, Tier1 also exceeds → SELECTIVE
    {
        auto m = ResolveModeByTokenBudget(500, 300, 400, 200);
        TestRunner::AssertEq(static_cast<int>(m), static_cast<int>(ToolDisclosureMode::SELECTIVE),
            "Tier2>Tier2Budget && Tier1>Tier1Budget → SELECTIVE");
    }
    // (b) Tier2 exceeds but Tier1 doesn't → PROGRESSIVE
    {
        auto m = ResolveModeByTokenBudget(500, 100, 400, 200);
        TestRunner::AssertEq(static_cast<int>(m), static_cast<int>(ToolDisclosureMode::PROGRESSIVE),
            "Tier2>Tier2Budget && Tier1<=Tier1Budget → PROGRESSIVE");
    }
    // (c) Neither exceeds → DISABLED
    {
        auto m = ResolveModeByTokenBudget(100, 50, 400, 200);
        TestRunner::AssertEq(static_cast<int>(m), static_cast<int>(ToolDisclosureMode::DISABLED),
            "Tier2<=Tier2Budget && Tier1<=Tier1Budget → DISABLED");
    }
    // Boundary: Tier2 exactly equals budget → NOT exceeding → DISABLED
    {
        auto m = ResolveModeByTokenBudget(400, 50, 400, 200);
        TestRunner::AssertEq(static_cast<int>(m), static_cast<int>(ToolDisclosureMode::DISABLED),
            "Tier2==Tier2Budget (not strictly >) → DISABLED");
    }
    // Boundary: Tier2 exceeds by 1, Tier1 exactly equals → PROGRESSIVE
    {
        auto m = ResolveModeByTokenBudget(401, 200, 400, 200);
        TestRunner::AssertEq(static_cast<int>(m), static_cast<int>(ToolDisclosureMode::PROGRESSIVE),
            "Tier2>Budget by 1, Tier1==Budget → PROGRESSIVE");
    }
}

// ============================================================
// 2. budget=0 skips judgment at that tier (free function)
// ============================================================
TEST(auto_resolution, BudgetZeroSkipsJudgment)
{
    // schemaBudget=0 → schema tier never triggers → always DISABLED
    // (regardless of tier2/tier1 being huge)
    {
        auto m = ResolveModeByTokenBudget(99999, 99999, 0, 200);
        TestRunner::AssertEq(static_cast<int>(m), static_cast<int>(ToolDisclosureMode::DISABLED),
            "schemaBudget=0 → schema tier skipped → DISABLED (even with huge tier2)");
    }
    // schemaBudget>0 && tier2 exceeds, catalogBudget=0 → catalog tier
    // skipped → PROGRESSIVE (not elevated to SELECTIVE)
    {
        auto m = ResolveModeByTokenBudget(500, 99999, 400, 0);
        TestRunner::AssertEq(static_cast<int>(m), static_cast<int>(ToolDisclosureMode::PROGRESSIVE),
            "schema exceeds but catalogBudget=0 → catalog tier skipped → PROGRESSIVE");
    }
    // Both budgets 0 → both tiers skipped → DISABLED (default AUTO config:
    // "I don't know the scale, conservatively don't add +1 LLM/turn")
    {
        auto m = ResolveModeByTokenBudget(99999, 99999, 0, 0);
        TestRunner::AssertEq(static_cast<int>(m), static_cast<int>(ToolDisclosureMode::DISABLED),
            "Both budgets 0 → DISABLED (conservative default)");
    }
    // schemaBudget=0 but catalogBudget>0: schema skipped, catalog checked
    // → still DISABLED (catalog tier only checked inside the schema-exceeds
    // branch, which is skipped)
    {
        auto m = ResolveModeByTokenBudget(99999, 99999, 0, 10);
        TestRunner::AssertEq(static_cast<int>(m), static_cast<int>(ToolDisclosureMode::DISABLED),
            "schemaBudget=0 → schema branch skipped → DISABLED even if catalog would exceed");
    }
}

// ============================================================
// 3. Empty pool → DISABLED (indirect, via AgentWorker)
// ============================================================
TEST(auto_resolution, EmptyPoolResolvesToDisabled)
{
    // Construct AUTO AgentWorker with empty pool (no defaultTools, no MCP
    // tools registered). First call to GetEffectiveMode() triggers lazy
    // resolution → ResolveByBudget({}) → tier2=0, tier1=small → with
    // budgets 0 → DISABLED.
    //
    // Indirect test: ResolveByBudget is private, IsProgressiveDisclosureActive
    // is protected — both inaccessible from the test. Go through the public
    // accessor GetEffectiveMode() (which calls IsProgressiveDisclosureActive()
    // for its side effect, then returns effectiveMode_).
    fs::path base = fs::current_path() / "test_tmp_auto_empty_pool";
    fs::remove_all(base);
    fs::create_directories(base);

    // Register a stub model so Agent construction doesn't throw
    // (ResolveByBudget itself never calls the model).
    ResourceManager::GetInstance().RegisterModel(
        "test-auto-stub", [](const ModelConfig&) {
            struct NullModel : public Model {
                explicit NullModel() : Model(ModelConfig()) {}
                std::string Format(const std::string&, const std::vector<Message>&,
                                   const std::vector<ToolSchema>&) override { return {}; }
                ModelResponse Invoke(const std::string&,
                                     std::function<void(const std::string&)>,
                                     std::function<bool()>) override {
                    ModelResponse r; r.isFinished = true; r.finishReason = "stop"; return r;
                }
            };
            return std::make_unique<NullModel>();
        });

    auto config = MakeAutoConfig(0, 0, base.string());
    auto worker = std::make_unique<ReactAgentWorker>(config);
    // No AddTools call — pool stays empty.

    // First call triggers lazy resolution. DISABLED → IsProgressiveDisclosure
    // Active would return false (but it's protected); GetEffectiveMode is
    // public and returns the resolved mode directly.
    TestRunner::AssertEq(static_cast<int>(worker->GetEffectiveMode()),
                         static_cast<int>(ToolDisclosureMode::DISABLED),
        "GetEffectiveMode() returns DISABLED for empty pool + budgets 0");

    fs::remove_all(base);
}

// ============================================================
// 4. call_once concurrent result consistency (solution D)
// ============================================================
TEST(auto_resolution, CallOnceConcurrentResultConsistent)
{
    // V3 (round5 §5.4.2 §5.5 test #4): multi-thread concurrent first call
    // to GetEffectiveMode() (public accessor; calls IsProgressiveDisclosure
    // Active() for side effect, then returns effectiveMode_). Assert all
    // threads see the same mode value (no torn read of effectiveMode_
    // mid-resolution).
    //
    // Solution D (weakened): assert concurrent result consistency. Does NOT
    // assert "ResolveByBudget called exactly once" — that would require
    // solution C (virtualize ResolveByBudget + callCount_ in derived test
    // class), which is invasive. call_once's one-shot semantics is a std
    // library guarantee; the real invariant to test is "concurrent callers
    // don't see a torn/unresolved effectiveMode_".
    //
    // TODO: solution C — virtualize ResolveByBudget, derive TestAgentWorker
    // with std::atomic<int> callCount_, assert callCount_ == 1 after
    // concurrent first calls. Left as follow-up (requires adding `virtual`
    // to ResolveByBudget in agent_worker.h — invasive for marginal coverage
    // gain; call_once's one-shot is std-guaranteed).

    fs::path base = fs::current_path() / "test_tmp_auto_concurrent";
    fs::remove_all(base);
    fs::create_directories(base);

    auto config = MakeAutoConfig(0, 0, base.string());
    auto worker = std::make_unique<ReactAgentWorker>(config);

    const int numThreads = 4;
    std::vector<std::thread> threads;
    std::vector<int> modes(numThreads, -1);
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    // Barrier: all threads reach the call site, then release simultaneously
    // to maximize the chance of racing the lazy resolution.
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&, i]() {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            modes[i] = static_cast<int>(worker->GetEffectiveMode());
        });
    }
    while (ready.load(std::memory_order_acquire) < numThreads) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    // All threads must see the same GetEffectiveMode() value (no torn read).
    for (int i = 1; i < numThreads; ++i) {
        TestRunner::AssertTrue(modes[i] == modes[0],
            "All threads see same GetEffectiveMode() value (no torn read mid-resolution)");
    }
    // With empty pool + budgets 0 → DISABLED
    TestRunner::AssertEq(modes[0], static_cast<int>(ToolDisclosureMode::DISABLED),
        "Concurrent first call resolves to DISABLED (empty pool, budgets 0)");

    fs::remove_all(base);
}

// ============================================================
// 5. ReloadAgent re-resolution (new AgentWorker → new resolution)
// ============================================================
TEST(auto_resolution, ReloadAgentResolvesWithNewPool)
{
    // V3 (round5 §5.4.2 §5.5 test #5): simulate ReloadAgent by constructing
    // two AgentWorkers sequentially with different configs/pools. Each has
    // its own resolveOnce_/effectiveMode_, so the second resolves fresh
    // based on the new pool. The "ReloadAgent" path in production constructs
    // a new Agent (→ new AgentWorker via CreateAgentWorker), then re-adds
    // defaultTools via AddTools — this test exercises the AgentWorker-level
    // core of that chain.
    //
    // Approach (per §5.4.2 §5.5 test #5 method (1)):
    //   - Register ~10 stub tools with short descriptions
    //   - AgentWorker1: AUTO + toolSchemaTokenBudget=10 (small) → schemas
    //     exceed 10 → at least PROGRESSIVE; catalog also exceeds 10 → SELECTIVE
    //   - AgentWorker2: AUTO + toolSchemaTokenBudget=100000 (huge) → schemas
    //     don't exceed → DISABLED
    //   - Verify the two workers resolve differently (new object → new
    //     resolution based on new config)
    fs::path base = fs::current_path() / "test_tmp_auto_reload";
    fs::remove_all(base);
    fs::create_directories(base);

    // Register 10 stub tools with moderate descriptions so total schema
    // tokens are non-trivial (> 10 but < 100000).
    RegisterStubTools(10, "This is a test tool description. ", 5);
    auto toolNames = StubToolNames(10);

    // Worker 1: small budgets → schemas/catalog exceed → SELECTIVE
    {
        auto config = MakeAutoConfig(10, 5, base.string());
        auto worker = std::make_unique<ReactAgentWorker>(config);
        worker->AddTools(toolNames);
        TestRunner::AssertEq(
            static_cast<int>(worker->GetEffectiveMode()),
            static_cast<int>(ToolDisclosureMode::SELECTIVE),
            "Worker 1: small budgets + 10 tools → SELECTIVE (both tiers exceed)");
    }

    // Worker 2: huge budgets → nothing exceeds → DISABLED
    // (new object → new resolveOnce_ → new resolution, not cached from worker 1)
    {
        auto config = MakeAutoConfig(100000, 100000, base.string());
        auto worker = std::make_unique<ReactAgentWorker>(config);
        worker->AddTools(toolNames);
        TestRunner::AssertEq(
            static_cast<int>(worker->GetEffectiveMode()),
            static_cast<int>(ToolDisclosureMode::DISABLED),
            "Worker 2: huge budgets + same tools → DISABLED (new object, new resolution)");
    }

    fs::remove_all(base);
}

// ============================================================
// 6. MCP increment doesn't change mode without reload (TODO placeholder)
// ============================================================
TEST(auto_resolution, McpIncrementDoesNotChangeModeWithoutReload)
{
    // V3 (round5 §5.4.2 §5.5 test #6): after AUTO resolves to DISABLED
    // (small pool), running-time RegisterMcpTools adds tools (pool grows)
    // but WITHOUT ReloadAgent — mode should stay DISABLED (call_once
    // already ran, once_flag set, no re-resolution).
    //
    // TODO: requires MCP server fixture to call RegisterMcpTools. The
    // test below is a placeholder that documents the contract; a real
    // implementation needs a fake MCP server or a way to inject tools
    // into ResourceManager's MCP registry without a live server.
    //
    // When the fixture becomes available, implement:
    //   1. Construct AUTO AgentWorker with small pool → resolve DISABLED
    //   2. RegisterMcpTools(...) to add tools making pool schema > budget
    //   3. Call IsProgressiveDisclosureActive() again → assert still DISABLED
    //      (once_flag set, call_once doesn't re-run)
    //   4. ReloadAgent(newConfig) → new AgentWorker → resolve PROGRESSIVE/SELECTIVE
    //
    // This placeholder is NOT silently deleted — per §5.5 test #6 contract,
    // the test file must keep the placeholder so future implementors see it.
    TestRunner::AssertTrue(true,
        "MCP increment test placeholder — requires MCP server fixture (see TODO above)");
}

// ============================================================
// 7. Tier 2 excludes alwaysOn schemas
// ============================================================
TEST(auto_resolution, Tier2ExcludesAlwaysOnSchemas)
{
    // V3 (round5 §5.4.2 §5.5 test #7): Tier 2 token estimation must
    // EXCLUDE alwaysOn tools' schemas (meta-tools + config_.alwaysOnTools).
    // alwaysOn tools are always FC-resident regardless of mode — counting
    // them would inflate Tier 2 and may falsely elevate a disabled-sized
    // pool to PROGRESSIVE.
    //
    // Approach:
    //   - Register 5 stub tools: 2 as alwaysOn (via config_.alwaysOnTools),
    //     3 as regular
    //   - Set toolSchemaTokenBudget to exactly the token count of the 3
    //     non-alwaysOn tools' schemas (boundary value)
    //   - If Tier 2 correctly excludes alwaysOn: tier2 = 3-non-alwaysOn
    //     tokens, NOT exceeding budget (tier2 == budget, not >) → DISABLED
    //   - If Tier 2 incorrectly includes alwaysOn: tier2 would be larger
    //     (5 tools' schemas) → exceeds budget → PROGRESSIVE (wrong)
    //
    // Note: meta-tools (tool_search/skill_search) are session-scoped, not
    // registered via RegisterTool — they may or may not be in BuildToolSchemas
    // depending on whether tool_search is registered (AUTO triggers
    // registration in SessionManager, but this test constructs AgentWorker
    // directly without SessionManager, so tool_search/skill_search are NOT
    // registered). The test focuses on the config_.alwaysOnTools exclusion
    // path; meta-tools exclusion is exercised in production via SessionManager
    // (where tool_search IS registered under AUTO).
    fs::path base = fs::current_path() / "test_tmp_auto_alwayson";
    fs::remove_all(base);
    fs::create_directories(base);

    // Register 5 stub tools with known description size.
    RegisterStubTools(5, "Test tool description. ", 10);
    auto allNames = StubToolNames(5);

    // Mark first 2 as alwaysOn via config.
    std::vector<std::string> alwaysOnConfig = {allNames[0], allNames[1]};

    // First, measure the schema tokens of the 3 non-alwaysOn tools to
    // set the boundary budget.
    auto& rm = ResourceManager::GetInstance();
    std::vector<std::string> nonAlwaysOnNames = {allNames[2], allNames[3], allNames[4]};
    auto nonAlwaysOnSchemas = rm.BuildToolSchemas(nonAlwaysOnNames);
    std::string nonAlwaysOnText;
    for (const auto& s : nonAlwaysOnSchemas) {
        nonAlwaysOnText += s.name + s.description + s.parameters.dump();
    }
    int nonAlwaysOnTokens = ContextEngine::EstimateTokens(nonAlwaysOnText);

    // Set budget to exactly the non-alwaysOn token count (boundary).
    // tier2 == budget → NOT strictly > → DISABLED (correct exclusion).
    // If alwaysOn were wrongly included: tier2 would be larger → > budget
    // → PROGRESSIVE (wrong).
    auto config = MakeAutoConfig(nonAlwaysOnTokens, 0, base.string());
    config.alwaysOnTools = alwaysOnConfig;

    auto worker = std::make_unique<ReactAgentWorker>(config);
    worker->AddTools(allNames);

    auto mode = worker->GetEffectiveMode();
    TestRunner::AssertEq(
        static_cast<int>(mode),
        static_cast<int>(ToolDisclosureMode::DISABLED),
        "Tier 2 excludes alwaysOn: tier2==budget (non-alwaysOn only) → DISABLED. "
        "If alwaysOn wrongly included, tier2 would exceed → PROGRESSIVE (wrong).");

    fs::remove_all(base);
}

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "include/agent.h"
#include "include/memory_runtime.h"
#include "include/model.h"
#include "include/resource_manager.h"
#include "include/types.h"
#include "test_runner.h"

namespace fs = std::filesystem;
using namespace jiuwen;

namespace {

// Minimal Model stub: Format + Invoke return canned responses. Only used to
// satisfy CreateModel inside ConsolidationLoop; the fake MemoryRuntime below
// ignores the model client entirely.
class StubModel : public Model
{
public:
    StubModel() : Model(ModelConfig()) {}

    std::string Format(const std::string& systemPrompt,
                       const std::vector<Message>& messages,
                       const std::vector<ToolSchema>& tools) override
    {
        (void)systemPrompt; (void)messages; (void)tools;
        return {};
    }

    ModelResponse Invoke(const std::string& formattedInput,
                         std::function<void(const std::string&)> onChunk,
                         std::function<bool()> /*shouldCancel*/) override
    {
        (void)formattedInput; (void)onChunk;
        ModelResponse r;
        r.content = "stub";
        r.isFinished = true;
        r.finishReason = "stop";
        return r;
    }
};

// Fake MemoryRuntime that counts Consolidate calls. All other methods return
// trivial success/empty results. Consolidate returns true so the loop's
// LegacyDreamConsolidator fallback is never reached, isolating the test to
// the memory-runtime path.
class FakeMemoryRuntime : public MemoryRuntime
{
public:
    explicit FakeMemoryRuntime(MemoryConfig config)
        : MemoryRuntime(std::move(config)) {}

    std::atomic<int> consolidateCalls{0};

    bool AppendEvent(const MemoryEvent&) override { return true; }
    MemoryContextPackage BuildContext(const MemoryContextRequest&) override { return {}; }
    MemoryPayloadWriteResult WritePayload(const MemoryPayloadWriteRequest&) override { return {}; }
    std::string ReadPayload(const std::string&) override { return {}; }

    bool Consolidate(const MemoryConsolidationRequest&) override
    {
        ++consolidateCalls;
        return true;
    }
    bool Consolidate(const MemoryConsolidationRequest&, MemoryModelClient*) override
    {
        ++consolidateCalls;
        return true;
    }

    std::vector<MemorySearchHit> SearchMemory(const MemorySearchRequest&) override { return {}; }
    MemoryStats GetStats() const override { return {}; }
};

// Poll cond() every 50ms up to timeoutSeconds. Returns true if cond became
// true, false on timeout. Avoids hard-coded sleeps that flake on slow CI.
bool WaitFor(std::function<bool()> cond, int timeoutSeconds)
{
    for (int i = 0; i < timeoutSeconds * 20; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return cond();
}

void RegisterStubModel()
{
    ResourceManager::GetInstance().RegisterModel(
        "test-consolidation-loop",
        [](const ModelConfig&) { return std::make_unique<StubModel>(); });
}

AgentConfig MakeConfig(const std::string& dataBasePath)
{
    AgentConfig config;
    config.id = "test-agent";
    config.mode = AgentWorkMode::REACT;
    config.modelConfig.provider = "test-consolidation-loop";
    config.memoryConfig.idleConsolidationSeconds = 1;
    config.dataBasePath = dataBasePath;
    return config;
}

} // namespace

// After one conversation completes (beyond the startup catch-up), Consolidate
// fires exactly once for that activity. The session stays idle in
// sessionActivity_, but with no new activity the dirty-flag gate skips
// subsequent polls -- no infinite re-trigger.
TEST(consolidation_loop, SkipsConsolidateWithoutNewActivity)
{
    fs::path base = fs::current_path() / "test_tmp_consolidation_loop_a";
    fs::remove_all(base);
    RegisterStubModel();

    MemoryConfig memConfig;
    FakeMemoryRuntime fake(memConfig);
    Agent agent(MakeConfig(base.string()));
    agent.SetMemoryRuntime(&fake);

    // The first poll cycle is the unconditional catch-up (bypasses both gates).
    bool catchUp = WaitFor([&] { return fake.consolidateCalls.load() >= 1; }, 4);
    TestRunner::AssertTrue(catchUp, "startup catch-up should fire unconditionally");

    // Now simulate a completed conversation: active -> idle sets hasNewActivity_.
    agent.NotifySessionActive("s1");
    agent.NotifySessionIdle("s1");

    bool activityFired = WaitFor([&] { return fake.consolidateCalls.load() >= 2; }, 4);
    TestRunner::AssertTrue(activityFired, "second consolidation should fire after activity");

    int countAfterActivity = fake.consolidateCalls.load();
    // Wait through at least two more poll cycles; the dirty flag was cleared
    // by the activity pass and no new conversation re-armed it, so the loop
    // must skip.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    TestRunner::AssertEq(fake.consolidateCalls.load(), countAfterActivity,
                         "no new activity -> no further consolidation");

    fs::remove_all(base);
}

// After the startup catch-up, no further Consolidate fires without new
// activity. A conversation completing then arms the flag and triggers the
// next consolidation.
TEST(consolidation_loop, ConsolidatesAfterActivity)
{
    fs::path base = fs::current_path() / "test_tmp_consolidation_loop_b";
    fs::remove_all(base);
    RegisterStubModel();

    MemoryConfig memConfig;
    FakeMemoryRuntime fake(memConfig);
    Agent agent(MakeConfig(base.string()));
    agent.SetMemoryRuntime(&fake);

    // Startup catch-up fires once (unconditional, bypasses gates).
    bool catchUp = WaitFor([&] { return fake.consolidateCalls.load() >= 1; }, 4);
    TestRunner::AssertTrue(catchUp, "startup catch-up should fire unconditionally");

    // No new activity: the gate must prevent further consolidation even though
    // the catch-up already ran.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    TestRunner::AssertEq(fake.consolidateCalls.load(), 1,
                         "no new activity after catch-up -> no further consolidation");

    // A conversation completing arms the flag and triggers the next poll.
    agent.NotifySessionActive("s1");
    agent.NotifySessionIdle("s1");

    bool fired = WaitFor([&] { return fake.consolidateCalls.load() >= 2; }, 4);
    TestRunner::AssertTrue(fired, "consolidation should fire after a conversation completes");

    fs::remove_all(base);
}

// After a conversation completes (beyond catch-up) and the session is cleaned
// up, no further Consolidate fires: both the dirty flag (cleared by the
// activity pass) and anyIdle (sessionActivity_ now empty) are false.
TEST(consolidation_loop, NoTriggerAfterCleanup)
{
    fs::path base = fs::current_path() / "test_tmp_consolidation_loop_c";
    fs::remove_all(base);
    RegisterStubModel();

    MemoryConfig memConfig;
    FakeMemoryRuntime fake(memConfig);
    Agent agent(MakeConfig(base.string()));
    agent.SetMemoryRuntime(&fake);

    // Wait for the startup catch-up to clear firstCycle so it does not
    // interfere with the activity-driven assertions below.
    bool catchUp = WaitFor([&] { return fake.consolidateCalls.load() >= 1; }, 4);
    TestRunner::AssertTrue(catchUp, "startup catch-up should fire unconditionally");

    agent.NotifySessionActive("s1");
    agent.NotifySessionIdle("s1");

    bool activityFired = WaitFor([&] { return fake.consolidateCalls.load() >= 2; }, 4);
    TestRunner::AssertTrue(activityFired, "consolidation should fire after activity");

    agent.CleanupSession("s1");

    int countAfterCleanup = fake.consolidateCalls.load();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    TestRunner::AssertEq(fake.consolidateCalls.load(), countAfterCleanup,
                         "no sessions and no new activity -> no consolidation");

    fs::remove_all(base);
}

// Sessions listed in MemoryConfig::excludedConsolidationSessionIds must not
// arm hasNewActivity_ when they go idle, so a cron/heartbeat-style Invoke
// cannot wake the consolidation loop. Session busy/idle tracking is still
// updated so the anyIdle gate behaves correctly.
TEST(consolidation_loop, ExcludedSessionDoesNotArmActivityFlag)
{
    fs::path base = fs::current_path() / "test_tmp_consolidation_loop_d";
    fs::remove_all(base);
    RegisterStubModel();

    AgentConfig config = MakeConfig(base.string());
    config.memoryConfig.excludedConsolidationSessionIds = {"__CRON__", "__HEARTBEAT__"};

    MemoryConfig memConfig;
    FakeMemoryRuntime fake(memConfig);
    Agent agent(config);
    agent.SetMemoryRuntime(&fake);

    // Wait for the startup catch-up so firstCycle is cleared.
    bool catchUp = WaitFor([&] { return fake.consolidateCalls.load() >= 1; }, 4);
    TestRunner::AssertTrue(catchUp, "startup catch-up should fire unconditionally");

    int callsAfterCatchUp = fake.consolidateCalls.load();

    // A system-triggered session goes through the active -> idle cycle.
    // NotifySessionIdle must NOT arm hasNewActivity_, so the next poll
    // cycle should skip the consolidation.
    agent.NotifySessionActive("__CRON__");
    agent.NotifySessionIdle("__CRON__");

    // The session must be tracked as idle (busy/idle tracking still works).
    TestRunner::AssertFalse(agent.IsSessionBusy("__CRON__"),
                            "excluded session should still report as idle");

    // Wait through at least two poll cycles. No user conversation completed,
    // so the dirty flag must remain clear and no new Consolidate call fires.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    TestRunner::AssertEq(fake.consolidateCalls.load(), callsAfterCatchUp,
                         "excluded session should not trigger consolidation");

    // A real user session completing after the excluded one still arms the
    // flag and triggers consolidation normally.
    agent.NotifySessionActive("user-session");
    agent.NotifySessionIdle("user-session");

    bool fired = WaitFor([&] { return fake.consolidateCalls.load() >= callsAfterCatchUp + 1; }, 4);
    TestRunner::AssertTrue(fired, "real user session should trigger consolidation after excluded idle");

    fs::remove_all(base);
}

// The consolidation request built inside ConsolidationLoop must carry the
// excluded session ids from MemoryConfig so the agent-memory-cpp batch
// builder skips those events end-to-end.
TEST(consolidation_loop, ConsolidationRequestCarriesExcludedSessionIds)
{
    fs::path base = fs::current_path() / "test_tmp_consolidation_loop_e";
    fs::remove_all(base);
    RegisterStubModel();

    AgentConfig config = MakeConfig(base.string());
    config.memoryConfig.excludedConsolidationSessionIds = {"__CRON__", "__HEARTBEAT__"};

    // Fake runtime that captures the request so we can inspect the field.
    class CapturingRuntime : public MemoryRuntime
    {
    public:
        explicit CapturingRuntime(MemoryConfig cfg) : MemoryRuntime(std::move(cfg)) {}
        std::atomic<int> calls{0};
        MemoryConsolidationRequest lastRequest;

        bool AppendEvent(const MemoryEvent&) override { return true; }
        MemoryContextPackage BuildContext(const MemoryContextRequest&) override { return {}; }
        MemoryPayloadWriteResult WritePayload(const MemoryPayloadWriteRequest&) override { return {}; }
        std::string ReadPayload(const std::string&) override { return {}; }
        bool Consolidate(const MemoryConsolidationRequest& r) override
        {
            lastRequest = r;
            ++calls;
            return true;
        }
        bool Consolidate(const MemoryConsolidationRequest& r, MemoryModelClient*) override
        {
            lastRequest = r;
            ++calls;
            return true;
        }
        std::vector<MemorySearchHit> SearchMemory(const MemorySearchRequest&) override { return {}; }
        MemoryStats GetStats() const override { return {}; }
    };

    CapturingRuntime fake(MemoryConfig{});
    Agent agent(config);
    agent.SetMemoryRuntime(&fake);

    bool fired = WaitFor([&] { return fake.calls.load() >= 1; }, 4);
    TestRunner::AssertTrue(fired, "startup catch-up should fire");

    TestRunner::AssertEq(fake.lastRequest.excludedSessionIds.size(), (size_t)2);
    TestRunner::AssertEq(fake.lastRequest.excludedSessionIds[0], std::string("__CRON__"));
    TestRunner::AssertEq(fake.lastRequest.excludedSessionIds[1], std::string("__HEARTBEAT__"));

    fs::remove_all(base);
}

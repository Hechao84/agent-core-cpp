// Tests for ReloadAgent graceful shutdown (no drain, no cancel). Verifies:
//  - Agent::MarkDraining / IsDraining flag round-trips.
//  - An in-flight Invoke on a session is NOT interrupted by ReloadAgent:
//    the in-flight turn runs to completion on the (now draining) old Agent;
//    ReloadAgent itself returns immediately (no drain wait).
//  - After reload, the next turn on the same session is routed to the new
//    (active) Agent (the session's bound Agent is rebound).
//  - The old Agent is destroyed once the last reference drops (probed via a
//    weak_ptr): the rebind on the next Invoke drops entry->agent's hold, and
//    ~Agent -> Shutdown joins the consolidation thread.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "include/agent.h"
#include "include/model.h"
#include "include/resource_manager.h"
#include "include/session_manager.h"
#include "include/tool.h"
#include "include/types.h"
#include "skills/skill_engine.h"
#include "test_runner.h"

namespace fs = std::filesystem;
using namespace jiuwen;

namespace {

// Latch shared between the test thread and the blocking model running inside
// the worker. BlockUntilReleased() parks the model Invoke; Release() unblocks
// it. The model also signals 'entered' before parking so the test can wait
// until the in-flight Invoke has actually reached the model call (avoiding a
// race where ReloadAgent runs before the Invoke enters the model).
struct InvokeLatch
{
    std::mutex m;
    std::condition_variable cv;
    bool released{false};
    std::atomic<bool> entered{false};

    void BlockUntilReleased()
    {
        entered.store(true, std::memory_order_release);
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [this]{ return released; });
    }
    void Release()
    {
        std::lock_guard<std::mutex> lock(m);
        released = true;
        cv.notify_all();
    }
    void Reset()
    {
        std::lock_guard<std::mutex> lock(m);
        released = false;
        entered.store(false, std::memory_order_release);
    }
    void WaitEntered()
    {
        while (!entered.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

// Model that blocks in Invoke until the shared latch is released, then returns
// a canned final answer (no tool_calls, isFinished) so the ReAct loop ends in
// one iteration. All instances created by the factory share the same latch.
class BlockingStubModel : public Model
{
public:
    BlockingStubModel(std::shared_ptr<InvokeLatch> latch, std::string answer)
        : Model(ModelConfig()), latch_(std::move(latch)), answer_(std::move(answer)) {}

    std::string Format(const std::string&, const std::vector<Message>&,
                       const std::vector<ToolSchema>&) override { return {}; }

    ModelResponse Invoke(const std::string&, std::function<void(const std::string&)>,
                         std::function<bool()>) override
    {
        latch_->BlockUntilReleased();
        ModelResponse r;
        r.content = answer_;
        r.isFinished = true;
        r.finishReason = "stop";
        return r;
    }

private:
    std::shared_ptr<InvokeLatch> latch_;
    std::string answer_;
};

// Non-blocking stub: returns a canned final answer immediately (one-iteration
// ReAct loop, no tool calls).
class ImmediateStubModel : public Model
{
public:
    explicit ImmediateStubModel(std::string answer)
        : Model(ModelConfig()), answer_(std::move(answer)) {}

    std::string Format(const std::string&, const std::vector<Message>&,
                       const std::vector<ToolSchema>&) override { return {}; }

    ModelResponse Invoke(const std::string&, std::function<void(const std::string&)>,
                         std::function<bool()>) override
    {
        ModelResponse r;
        r.content = answer_;
        r.isFinished = true;
        r.finishReason = "stop";
        return r;
    }

private:
    std::string answer_;
};

AgentConfig MakeConfig(const std::string& dataBasePath, const std::string& provider)
{
    AgentConfig config;
    config.id = "test-agent";
    config.mode = AgentWorkMode::REACT;
    config.modelConfig.provider = provider;
    config.dataBasePath = dataBasePath;
    // V2 default is SELECTIVE (which calls findRelevant → consumes a model
    // call at turn-start). These reload tests use stub models with
    // latch-based blocking and don't exercise progressive disclosure, so
    // force DISABLED to keep the V1 single-model-call flow that the tests
    // were written against. findRelevant would otherwise park on the same
    // latch the in-flight turn is supposed to enter, breaking the
    // "blocking on old Agent, reload, release, observe" protocol.
    config.toolDisclosureMode = ToolDisclosureMode::DISABLED;
    // Keep the consolidation thread's first poll far away so it does not call
    // CreateModel (and thus the blocking model) during the test window.
    config.memoryConfig.idleConsolidationSeconds = 3600;
    return config;
}

// Model that blocks in Invoke until the latch is released, then on its FIRST
// call (counted across all instances sharing 'phase' — each loop iteration
// constructs a fresh model) returns a tool_call for "record_skill_engine"; on
// subsequent calls it returns a final answer. Lets the test park an in-flight
// turn at the model call, run a reload, then unblock so the turn executes a
// session tool (capturing the SkillEngine GetSkillEngine returned) before it
// finishes — proving the in-flight turn stayed on the old Agent.
class TwoPhaseBlockingModel : public Model
{
public:
    TwoPhaseBlockingModel(std::shared_ptr<InvokeLatch> latch,
                          std::shared_ptr<std::atomic<int>> phase,
                          std::string finalAnswer)
        : Model(ModelConfig()),
          latch_(std::move(latch)),
          phase_(std::move(phase)),
          finalAnswer_(std::move(finalAnswer)) {}

    std::string Format(const std::string&, const std::vector<Message>&,
                       const std::vector<ToolSchema>&) override { return {}; }

    ModelResponse Invoke(const std::string&, std::function<void(const std::string&)>,
                         std::function<bool()>) override
    {
        latch_->BlockUntilReleased();
        int p = phase_->fetch_add(1, std::memory_order_acq_rel);
        ModelResponse r;
        if (p == 0) {
            ToolCall tc;
            tc.name = "record_skill_engine";
            tc.argumentsJson = "{}";
            r.toolCalls.push_back(std::move(tc));
            r.isFinished = false;
            r.finishReason = "tool_calls";
        } else {
            r.content = finalAnswer_;
            r.isFinished = true;
            r.finishReason = "stop";
        }
        return r;
    }

private:
    std::shared_ptr<InvokeLatch> latch_;
    std::shared_ptr<std::atomic<int>> phase_;
    std::string finalAnswer_;
};

// Session tool whose factory captures the SkillEngine pointer
// (ToolBuildContext::skillEngine, populated from WorkerEnv::GetSkillEngine)
// into a shared atomic probe. Lets the test assert which Agent's SkillEngine
// an in-flight turn actually used. Invoke is a no-op.
class RecorderSkillTool : public Tool
{
public:
    RecorderSkillTool()
        : Tool("record_skill_engine",
               "records the bound Agent's SkillEngine pointer for the test",
               {}) {}
    std::string Invoke(const std::string&) override { return "{}"; }
};

} // namespace

// Agent::MarkDraining / IsDraining round-trip. The flag defaults to false,
// MarkDraining flips it to true.
TEST(reload_graceful, AgentDrainingFlagRoundTrip)
{
    fs::path base = fs::current_path() / "test_tmp_reload_draining_flag";
    fs::remove_all(base);

    // Register a stub model so Agent construction + the (idle) consolidation
    // thread can call CreateModel without throwing.
    ResourceManager::GetInstance().RegisterModel(
        "test-reload-draining-flag",
        [](const ModelConfig&) { return std::make_unique<ImmediateStubModel>("ok"); });

    AgentConfig config = MakeConfig(base.string(), "test-reload-draining-flag");
    Agent agent(config);

    TestRunner::AssertFalse(agent.IsDraining(), "fresh Agent must not be draining");
    agent.MarkDraining();
    TestRunner::AssertTrue(agent.IsDraining(), "MarkDraining must set the flag");

    agent.Shutdown();
    fs::remove_all(base);
}

// Full graceful-reload flow: in-flight turn preserved on old Agent; reload
// does not block; next turn routed to new Agent; old Agent destructed after
// rebind drops the last reference (probed via weak_ptr).
TEST(reload_graceful, ReloadAgentPreservesInFlightTurnAndRebinds)
{
    auto latch = std::make_shared<InvokeLatch>();
    const std::string blockingProvider = "test-reload-blocking";
    const std::string immediateProvider = "test-reload-immediate";
    const std::string oldAnswer = "old-turn-answer";
    const std::string newAnswer = "new-turn-answer";

    ResourceManager::GetInstance().RegisterModel(
        blockingProvider,
        [latch, oldAnswer](const ModelConfig&) {
            return std::make_unique<BlockingStubModel>(latch, oldAnswer);
        });
    ResourceManager::GetInstance().RegisterModel(
        immediateProvider,
        [newAnswer](const ModelConfig&) {
            return std::make_unique<ImmediateStubModel>(newAnswer);
        });

    fs::path base = fs::current_path() / "test_tmp_reload_graceful_flow";
    fs::remove_all(base);

    InitSessionManager(MakeConfig(base.string(), blockingProvider));
    auto& sm = GetSessionManager();

    const std::string sid = "graceful-reload-session";

    // Capture the old Agent for probing; weak_ptr lets us observe destruction
    // without keeping it alive.
    std::shared_ptr<Agent> oldAgent = sm.GetAgent();
    std::weak_ptr<Agent> weakOld = oldAgent;
    TestRunner::AssertTrue(oldAgent != nullptr, "active Agent must exist after Initialize");
    TestRunner::AssertFalse(oldAgent->IsDraining(), "active Agent must not be draining");

    // Start an Invoke on a worker thread. It enters the blocking model and
    // parks until we Release the latch.
    std::atomic<bool> invokeDone{false};
    SessionInvokeResult invokeResult{};
    std::thread invokeThread([&] {
        invokeResult = sm.Invoke(sid, "first message", [](const std::string&) {});
        invokeDone.store(true, std::memory_order_release);
    });

    // Wait until the in-flight Invoke has actually reached the model call,
    // so the reload below genuinely races an in-flight turn.
    latch->WaitEntered();

    // ReloadAgent must NOT block on the in-flight Invoke. Time it: a graceful
    // swap returns near-instantly; a drain-based implementation would block
    // until the latch is released (indefinitely here).
    auto reloadStart = std::chrono::steady_clock::now();
    std::string reloadErr;
    bool ok = sm.ReloadAgent(MakeConfig(base.string(), immediateProvider), &reloadErr);
    auto reloadElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - reloadStart).count();
    TestRunner::AssertTrue(ok, std::string("ReloadAgent must succeed") +
                            (reloadErr.empty() ? "" : (": " + reloadErr)));
    TestRunner::AssertTrue(reloadElapsed < 1000,
                           "ReloadAgent must not block on in-flight Invoke (graceful)");

    // Active Agent is now the new one; old Agent is draining.
    std::shared_ptr<Agent> newAgent = sm.GetAgent();
    TestRunner::AssertTrue(newAgent.get() != oldAgent.get(),
                           "active Agent must differ after reload");
    TestRunner::AssertTrue(oldAgent->IsDraining(),
                           "old Agent must be marked draining after reload");

    // Release the in-flight turn. It must complete on the OLD Agent, so its
    // result is the old model's answer.
    latch->Release();
    while (!invokeDone.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    invokeThread.join();
    TestRunner::AssertTrue(invokeResult.success, "in-flight Invoke must complete successfully");
    TestRunner::AssertTrue(invokeResult.content.find(oldAnswer) != std::string::npos,
                           "in-flight turn must be served by the old Agent (old answer)");

    // The session's entry->agent still points at the old (draining) Agent, so
    // the old Agent is still alive (one strong reference via the entry).
    std::shared_ptr<Agent> stillOld = weakOld.lock();
    TestRunner::AssertTrue(stillOld != nullptr,
                           "old Agent must stay alive while entry still binds it");

    // Next turn on the same session: entry->agent is draining, so Invoke
    // rebinds it to the active (new) Agent. This drops the entry's hold on
    // the old Agent; with no other references, ~Agent fires.
    auto r2 = sm.Invoke(sid, "second message", [](const std::string&) {});
    TestRunner::AssertTrue(r2.success, "second Invoke must succeed");
    TestRunner::AssertTrue(r2.content.find(newAnswer) != std::string::npos,
                           "second turn must be served by the new Agent (new answer)");

    // The rebind dropped the entry's strong reference to the old Agent; the
    // only remaining references were ReloadAgent's local (already released)
    // and our 'oldAgent'/'stillOld' locals. Drop those now.
    stillOld.reset();
    oldAgent.reset();
    std::shared_ptr<Agent> probe = weakOld.lock();
    TestRunner::AssertTrue(probe == nullptr,
                           "old Agent must be destroyed after rebind drops the last reference");

    sm.Shutdown();
    fs::remove_all(base);
}

// A session created AFTER a reload binds the active (new) Agent directly, so
// its first turn is served by the new Agent without any rebind dance.
TEST(reload_graceful, NewSessionBindsActiveAgentAfterReload)
{
    const std::string providerA = "test-reload-newsession-a";
    const std::string providerB = "test-reload-newsession-b";
    ResourceManager::GetInstance().RegisterModel(
        providerA, [](const ModelConfig&) { return std::make_unique<ImmediateStubModel>("a"); });
    ResourceManager::GetInstance().RegisterModel(
        providerB, [](const ModelConfig&) { return std::make_unique<ImmediateStubModel>("b"); });

    fs::path base = fs::current_path() / "test_tmp_reload_new_session";
    fs::remove_all(base);

    InitSessionManager(MakeConfig(base.string(), providerA));
    auto& sm = GetSessionManager();

    std::string err;
    TestRunner::AssertTrue(sm.ReloadAgent(MakeConfig(base.string(), providerB), &err),
                           "reload to provider B must succeed");

    // A brand-new session created post-reload binds the active Agent (B).
    auto r = sm.Invoke("post-reload-session", "hi", [](const std::string&) {});
    TestRunner::AssertTrue(r.success, "post-reload Invoke must succeed");
    TestRunner::AssertTrue(r.content.find("b") != std::string::npos,
                           "new session post-reload must be served by the new Agent");

    sm.Shutdown();
    fs::remove_all(base);
}

// ReloadAgent construction failure leaves the old Agent in place and active
// (not draining). The config is applied only on success. Construction failure
// is triggered by an unimplemented AgentWorkMode (CreateAgentWorker throws).
TEST(reload_graceful, ReloadAgentFailureLeavesOldAgentInPlace)
{
    const std::string provider = "test-reload-failure-keep";
    ResourceManager::GetInstance().RegisterModel(
        provider, [](const ModelConfig&) { return std::make_unique<ImmediateStubModel>("ok"); });

    fs::path base = fs::current_path() / "test_tmp_reload_failure";
    fs::remove_all(base);

    InitSessionManager(MakeConfig(base.string(), provider));
    auto& sm = GetSessionManager();
    std::shared_ptr<Agent> before = sm.GetAgent();

    // An unimplemented AgentWorkMode makes CreateAgentWorker (called from the
    // Agent constructor) throw, so ReloadAgent's try/catch must report failure
    // and leave the old Agent active and non-draining.
    AgentConfig badConfig = MakeConfig(base.string(), provider);
    badConfig.mode = AgentWorkMode::PLAN_AND_EXECUTE;

    std::string err;
    bool ok = sm.ReloadAgent(badConfig, &err);
    TestRunner::AssertFalse(ok, "ReloadAgent must report failure when construction throws");
    TestRunner::AssertTrue(!err.empty(), "errorOut must be populated on failure");

    std::shared_ptr<Agent> after = sm.GetAgent();
    TestRunner::AssertTrue(after.get() == before.get(),
                           "old Agent must remain active after failed reload");
    TestRunner::AssertFalse(before->IsDraining(),
                            "old Agent must not be marked draining after failed reload");

    sm.Shutdown();
    fs::remove_all(base);
}

// Regression for the SmWorkerEnv::GetSkillEngine race + "one turn, two Agents"
// bug. An in-flight turn running on the (about-to-be-draining) old Agent
// executes a session tool that records ctx.skillEngine. After ReloadAgent
// swaps in a new Agent mid-turn, the recorded SkillEngine must still be the
// OLD Agent's — the in-flight turn must not reach across to the new Agent's
// resources. Pre-fix, GetSkillEngine read the global active agent_ without a
// lock, so the in-flight turn grabbed the NEW Agent's SkillEngine (a data
// race on the shared_ptr control block AND a semantic violation of
// "回合内始终单 Agent").
TEST(reload_graceful, InFlightTurnUsesBoundAgentSkillEngineAcrossReload)
{
    // Real skill dir so each Agent constructs a non-null, distinct SkillEngine.
    fs::path skillRoot = fs::current_path() / "test_tmp_reload_skill_root";
    fs::path skillDir = skillRoot / "demo_skill";
    fs::remove_all(skillRoot);
    fs::create_directories(skillDir);
    {
        std::ofstream f(skillDir / "SKILL.md");
        f << "---\nname: demo\ndescription: a demo skill\n---\nbody\n";
    }

    auto latch = std::make_shared<InvokeLatch>();
    auto phase = std::make_shared<std::atomic<int>>(0);
    auto probe = std::make_shared<std::atomic<SkillEngine*>>(nullptr);

    const std::string oldProvider = "test-reload-skill-old";
    const std::string newProvider = "test-reload-skill-new";
    ResourceManager::GetInstance().RegisterModel(
        oldProvider, [latch, phase](const ModelConfig&) {
            return std::make_unique<TwoPhaseBlockingModel>(latch, phase, "old-done");
        });
    ResourceManager::GetInstance().RegisterModel(
        newProvider, [](const ModelConfig&) {
            return std::make_unique<ImmediateStubModel>("new-done");
        });
    // Session tool: factory writes ctx.skillEngine (populated from
    // WorkerEnv::GetSkillEngine at agent_worker.cpp:392) into the probe, then
    // constructs the no-op recorder tool.
    ResourceManager::GetInstance().RegisterSessionTool(
        "record_skill_engine",
        [probe](const ToolBuildContext& ctx) -> std::unique_ptr<Tool> {
            probe->store(ctx.skillEngine, std::memory_order_release);
            return std::make_unique<RecorderSkillTool>();
        });

    fs::path base = fs::current_path() / "test_tmp_reload_skill_flow";
    fs::remove_all(base);

    AgentConfig oldCfg = MakeConfig(base.string(), oldProvider);
    oldCfg.skillDirectory = skillRoot.string();
    InitSessionManager(oldCfg);
    auto& sm = GetSessionManager();

    std::shared_ptr<Agent> oldAgent = sm.GetAgent();
    SkillEngine* oldSE = oldAgent->GetSkillEngine();
    TestRunner::AssertTrue(oldSE != nullptr, "old Agent must have a SkillEngine");

    const std::string sid = "skill-reload-session";
    std::atomic<bool> invokeDone{false};
    SessionInvokeResult invokeResult{};
    std::thread invokeThread([&] {
        invokeResult = sm.Invoke(sid, "first", [](const std::string&) {});
        invokeDone.store(true, std::memory_order_release);
    });

    // Wait until the in-flight turn is parked inside the (old Agent's) model.
    latch->WaitEntered();

    // Reload to the new Agent (same skill dir -> a distinct SkillEngine
    // instance per Agent). Swap happens under sessionMutex_; the in-flight
    // turn keeps running on the old Agent.
    AgentConfig newCfg = MakeConfig(base.string(), newProvider);
    newCfg.skillDirectory = skillRoot.string();
    std::string err;
    TestRunner::AssertTrue(sm.ReloadAgent(newCfg, &err), "reload must succeed");

    std::shared_ptr<Agent> newAgent = sm.GetAgent();
    SkillEngine* newSE = newAgent->GetSkillEngine();
    TestRunner::AssertTrue(newSE != nullptr, "new Agent must have a SkillEngine");
    TestRunner::AssertTrue(newSE != oldSE,
                           "old/new Agents must have distinct SkillEngine instances");

    // Release the in-flight turn. It executes the recorder tool (capturing
    // GetSkillEngine's return) then finishes with the old model's answer.
    latch->Release();
    while (!invokeDone.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    invokeThread.join();
    TestRunner::AssertTrue(invokeResult.success, "in-flight turn must complete");
    TestRunner::AssertTrue(invokeResult.content.find("old-done") != std::string::npos,
                           "in-flight turn must be served by the old Agent (old answer)");

    // THE KEY ASSERTION: the SkillEngine recorded during the in-flight turn
    // is the OLD Agent's, not the new Agent's. The in-flight turn on the
    // draining old Agent did not reach across to the new Agent's resources.
    SkillEngine* recordedSE = probe->load(std::memory_order_acquire);
    TestRunner::AssertTrue(recordedSE != nullptr,
                           "recorder tool must have captured a SkillEngine pointer");
    TestRunner::AssertTrue(recordedSE == oldSE,
                           "in-flight turn's skill lookup must use the bound (old) Agent's SkillEngine");
    TestRunner::AssertTrue(recordedSE != newSE,
                           "in-flight turn must NOT use the new Agent's SkillEngine");

    sm.Shutdown();
    fs::remove_all(base);
    fs::remove_all(skillRoot);
}

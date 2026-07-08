// Tests for SessionManager's reserved-session registry and RemoveSession
// protection. Verifies that:
//  - The core library auto-registers __DEFAULT__ at construction so it
//    cannot be deleted.
//  - Application layers can register additional ids via
//    RegisterReservedSession; RemoveSession refuses to delete any
//    registered id.
//  - Unregistered ids are deleted normally.

#include <filesystem>
#include <memory>
#include <string>

#include "include/agent.h"
#include "include/resource_manager.h"
#include "include/session_manager.h"
#include "include/types.h"
#include "test_runner.h"

namespace fs = std::filesystem;
using namespace jiuwen;

namespace {

// Minimal agent config so SessionManager::Initialize succeeds without a real
// model. The model provider is a stub registered below.
class StubModel : public Model
{
public:
    StubModel() : Model(ModelConfig()) {}

    std::string Format(const std::string&,
                       const std::vector<Message>&,
                       const std::vector<ToolSchema>&) override
    {
        return {};
    }

    ModelResponse Invoke(const std::string&,
                         std::function<void(const std::string&)>,
                         std::function<bool()>) override
    {
        ModelResponse r;
        r.content = "stub";
        r.isFinished = true;
        r.finishReason = "stop";
        return r;
    }
};

void RegisterStubModel()
{
    static const std::string provider = "test-session-manager-stub";
    ResourceManager::GetInstance().RegisterModel(
        provider, [](const ModelConfig&) { return std::make_unique<StubModel>(); });
}

AgentConfig MakeConfig(const std::string& dataBasePath)
{
    AgentConfig config;
    config.id = "test-agent";
    config.mode = AgentWorkMode::REACT;
    config.modelConfig.provider = "test-session-manager-stub";
    config.dataBasePath = dataBasePath;
    return config;
}

} // namespace

// kDefaultSessionId is auto-registered by the core library at SessionManager
// construction. RemoveSession must refuse to delete it.
TEST(session_manager, DefaultSessionIsAutoReserved)
{
    RegisterStubModel();
    fs::path base = fs::temp_directory_path() / "jiuwen_sm_default_reserved";
    fs::remove_all(base);

    InitSessionManager(MakeConfig(base.string()));
    auto& sm = GetSessionManager();

    // __DEFAULT__ is created during Initialize and must be reserved.
    auto ids = sm.GetSessionIds();
    bool hasDefault = false;
    for (const auto& id : ids) {
        if (id == kDefaultSessionId) hasDefault = true;
    }
    TestRunner::AssertTrue(hasDefault, "__DEFAULT__ should exist after Initialize");

    // RemoveSession is a no-op on reserved ids: __DEFAULT__ must still exist.
    sm.RemoveSession(kDefaultSessionId);
    ids = sm.GetSessionIds();
    bool stillHasDefault = false;
    for (const auto& id : ids) {
        if (id == kDefaultSessionId) stillHasDefault = true;
    }
    TestRunner::AssertTrue(stillHasDefault, "reserved __DEFAULT__ should not be deleted");

    sm.Shutdown();
    fs::remove_all(base);
}

// RegisterReservedSession protects an application-declared system session id
// from deletion, while unregistered ids are deleted normally.
TEST(session_manager, RegisterReservedSessionProtectsFromDeletion)
{
    RegisterStubModel();
    fs::path base = fs::temp_directory_path() / "jiuwen_sm_register_reserved";
    fs::remove_all(base);

    InitSessionManager(MakeConfig(base.string()));
    auto& sm = GetSessionManager();

    // Register a system session like jiuwenClaw would do for cron/heartbeat.
    sm.RegisterReservedSession("__HEARTBEAT__");
    sm.RegisterReservedSession("__CRON__");

    // Pre-create the system session so we can verify RemoveSession is a no-op.
    sm.GetOrCreateSession("__HEARTBEAT__");
    sm.GetOrCreateSession("__CRON__");
    sm.GetOrCreateSession("user-temp");

    // Reserved ids survive RemoveSession.
    sm.RemoveSession("__HEARTBEAT__");
    sm.RemoveSession("__CRON__");
    auto ids = sm.GetSessionIds();
    bool hasHb = false, hasCron = false;
    for (const auto& id : ids) {
        if (id == "__HEARTBEAT__") hasHb = true;
        if (id == "__CRON__") hasCron = true;
    }
    TestRunner::AssertTrue(hasHb, "reserved __HEARTBEAT__ should not be deleted");
    TestRunner::AssertTrue(hasCron, "reserved __CRON__ should not be deleted");

    // Unregistered id is deleted normally.
    sm.RemoveSession("user-temp");
    ids = sm.GetSessionIds();
    bool hasUser = false;
    for (const auto& id : ids) {
        if (id == "user-temp") hasUser = true;
    }
    TestRunner::AssertFalse(hasUser, "unregistered session should be deleted");

    // Empty id is a no-op (does not throw, does not corrupt state).
    sm.RemoveSession("");

    sm.Shutdown();
    fs::remove_all(base);
}

// RegisterReservedSession is idempotent: registering the same id twice does
// not throw or duplicate entries.
TEST(session_manager, RegisterReservedSessionIsIdempotent)
{
    RegisterStubModel();
    fs::path base = fs::temp_directory_path() / "jiuwen_sm_register_idempotent";
    fs::remove_all(base);

    InitSessionManager(MakeConfig(base.string()));
    auto& sm = GetSessionManager();

    sm.RegisterReservedSession("__HEARTBEAT__");
    sm.RegisterReservedSession("__HEARTBEAT__");  // duplicate must be safe
    sm.RegisterReservedSession("");  // empty must be ignored

    sm.GetOrCreateSession("__HEARTBEAT__");
    sm.RemoveSession("__HEARTBEAT__");
    auto ids = sm.GetSessionIds();
    bool stillHasHb = false;
    for (const auto& id : ids) {
        if (id == "__HEARTBEAT__") stillHasHb = true;
    }
    TestRunner::AssertTrue(stillHasHb, "duplicate registration should still protect the id");

    sm.Shutdown();
    fs::remove_all(base);
}

// Explicit positive-case counterpart to the reserved-protection tests:
// an unregistered session is deleted normally (disappears from
// GetSessionIds and its on-disk directory is removed). The other tests
// only assert this implicitly by comparing reserved vs unregistered
// behavior; this test asserts the unregistered path directly so the
// intent is unambiguous.
TEST(session_manager, UnregisteredSessionCanBeDeleted)
{
    RegisterStubModel();
    fs::path base = fs::temp_directory_path() / "jiuwen_sm_unregistered_delete";
    fs::remove_all(base);

    InitSessionManager(MakeConfig(base.string()));
    auto& sm = GetSessionManager();

    // Do NOT register this id -- it is a plain user session.
    const std::string userSession = "plain-user-session";
    sm.GetOrCreateSession(userSession);

    // Sanity: present before delete.
    auto idsBefore = sm.GetSessionIds();
    bool foundBefore = false;
    for (const auto& id : idsBefore) {
        if (id == userSession) foundBefore = true;
    }
    TestRunner::AssertTrue(foundBefore, "session should exist after GetOrCreateSession");

    // Delete and verify it is gone from the in-memory registry.
    sm.RemoveSession(userSession);
    auto idsAfter = sm.GetSessionIds();
    bool foundAfter = false;
    for (const auto& id : idsAfter) {
        if (id == userSession) foundAfter = true;
    }
    TestRunner::AssertFalse(foundAfter, "unregistered session should be deleted from registry");

    // On-disk directory should also be gone (no busy Invoke, so disk
    // deletion is not skipped).
    fs::path sessionDir = fs::path(base) / "sessions" / userSession;
    TestRunner::AssertTrue(!fs::exists(sessionDir),
                           "unregistered session directory should be removed from disk");

    sm.Shutdown();
    fs::remove_all(base);
}

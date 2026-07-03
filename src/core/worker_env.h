#pragma once

#include <memory>
#include <string>

namespace jiuwen {

class ContextEngine;
class SessionTodoList;
class AskUserDispatcher;
class MemoryRuntime;
class SkillEngine;

struct SessionEntry;

// WorkerEnv decouples AgentWorker from the concrete resource owners.
// The implementation resolves session-scoped resources (ContextEngine,
// todo list, ask_user dispatcher) by sessionId and returns the global
// memory runtime. No back-reference to Agent — the cycle
// AgentWorker → WorkerEnv → Agent is eliminated.
//
// During an Invoke call, SessionManager pre-fetches the session entry
// and sets it via SetCurrentEntry so that subsequent lookups don't need
// to acquire sessionMutex_ (eliminating invokeMutex→sessionMutex_
// nesting per the lock ordering protocol). ClearCurrentEntry is called
// after Invoke completes.
class WorkerEnv {
public:
    virtual ~WorkerEnv() = default;

    virtual std::shared_ptr<ContextEngine> GetContextEngine(const std::string& sessionId) = 0;
    virtual SessionTodoList* GetOrCreateSessionTodoList(const std::string& sessionId) = 0;
    virtual AskUserDispatcher* GetAskUserDispatcher(const std::string& sessionId) = 0;
    virtual MemoryRuntime* GetMemoryRuntime() = 0;
    // Returns the active Agent's SkillEngine (Agent-scoped, shared across
    // sessions; non-owning). Resolves through the current Agent so it stays
    // valid across ReloadAgent swaps.
    virtual SkillEngine* GetSkillEngine() = 0;

    virtual void SetCurrentEntry(std::shared_ptr<SessionEntry> entry) = 0;
    virtual void ClearCurrentEntry() = 0;
};

} // namespace jiuwen

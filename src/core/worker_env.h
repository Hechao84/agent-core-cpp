#pragma once

#include <memory>
#include <string>

namespace jiuwen {

class ContextEngine;
class SessionTodoList;
class AskUserDispatcher;
class MemoryRuntime;
class SkillEngine;
class ToolTurnState;

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

    // Returns the per-turn tool disclosure state for the current session
    // (the ToolTurnState proxy owned by the current SessionEntry). Both
    // AgentWorker::BuildToolSchemas (reads loadedTools to rebuild FC =
    // alwaysOn ∪ loadedTools each iteration) and the tool_search tool
    // (writes loadedTools/activeSet via ToolBuildContext.turnState) must
    // reach the SAME per-turn state object — this returns it.
    //
    // Single-path (not the GetSkillEngine() double-path): if tlCurrentEntry_
    // is set, lazily constructs the proxy on it (once per SessionEntry) and
    // returns it; if tlCurrentEntry_ is null (outside an Invoke), returns
    // nullptr. turnState is per-SessionEntry (not per-Agent like SkillEngine),
    // so there is no global fallback to query when no current entry exists.
    virtual ToolTurnState* GetCurrentTurnState() = 0;
};

} // namespace jiuwen

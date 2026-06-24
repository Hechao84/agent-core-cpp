#pragma once

#include <string>

namespace jiuwen {

class SessionTodoList;
class AskUserDispatcher;
class MemoryRuntime;

// WorkerEnv decouples AgentWorker from the concrete resource owners.
// The implementation resolves session-scoped resources (todo list,
// ask_user dispatcher) by sessionId and returns the global memory runtime.
// No back-reference to Agent — the cycle AgentWorker → WorkerEnv → Agent
// No back-reference to Agent — the cycle AgentWorker → WorkerEnv → Agent
// is eliminated.
class WorkerEnv {
public:
    virtual ~WorkerEnv() = default;

    virtual SessionTodoList* GetOrCreateSessionTodoList(const std::string& sessionId) = 0;
    virtual AskUserDispatcher* GetAskUserDispatcher(const std::string& sessionId) = 0;
    virtual MemoryRuntime* GetMemoryRuntime() = 0;
};

} // namespace jiuwen

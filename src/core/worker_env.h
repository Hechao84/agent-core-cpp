#pragma once

#include <string>

namespace jiuwen {

class SessionTodoList;
class AskUserDispatcher;

// WorkerEnv decouples AgentWorker from Agent. Agent implements this interface
// privately so the worker can obtain session-scoped resources without
// depending on the full Agent type.
class WorkerEnv {
public:
    virtual ~WorkerEnv() = default;

    virtual SessionTodoList* GetOrCreateSessionTodoList(const std::string& sessionId) = 0;
    virtual AskUserDispatcher* GetAskUserDispatcher() = 0;
};

} // namespace jiuwen

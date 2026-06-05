#pragma once

#include <string>
#include "include/tool.h"

namespace jiuwen {

class SessionTodoList;

// Each Todo* tool operates on an injected SessionTodoList* and is
// completely session-agnostic. The Agent owns the per-session list and
// hands the right pointer to the tool factory at construction time.

class TodoCreateTool : public Tool {
public:
    explicit TodoCreateTool(SessionTodoList* list);
    std::string Invoke(const std::string& input) override;
private:
    SessionTodoList* list_;
};

class TodoCompleteTool : public Tool {
public:
    explicit TodoCompleteTool(SessionTodoList* list);
    std::string Invoke(const std::string& input) override;
private:
    SessionTodoList* list_;
};

class TodoInsertTool : public Tool {
public:
    explicit TodoInsertTool(SessionTodoList* list);
    std::string Invoke(const std::string& input) override;
private:
    SessionTodoList* list_;
};

class TodoRemoveTool : public Tool {
public:
    explicit TodoRemoveTool(SessionTodoList* list);
    std::string Invoke(const std::string& input) override;
private:
    SessionTodoList* list_;
};

class TodoListTool : public Tool {
public:
    explicit TodoListTool(SessionTodoList* list);
    std::string Invoke(const std::string& input) override;
private:
    SessionTodoList* list_;
};

} // namespace jiuwen

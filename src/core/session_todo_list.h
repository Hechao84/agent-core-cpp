#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace jiuwen {

struct TodoItem
{
    std::string content;
    std::string status;
    std::string result;
};

class SessionTodoList {
public:
    SessionTodoList() = default;

    void Create(const std::vector<std::string>& contents);
    bool Complete(int index, const std::string& result);
    bool Insert(int index, const std::string& content);
    bool Remove(int index);
    std::vector<TodoItem> List() const;
    bool Empty() const;
    std::string Render() const;

private:
    mutable std::mutex mu_;
    std::vector<TodoItem> items_;
};

} // namespace jiuwen

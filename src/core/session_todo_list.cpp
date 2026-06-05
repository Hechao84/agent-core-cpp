#include "src/core/session_todo_list.h"

#include <sstream>

namespace jiuwen {

void SessionTodoList::Create(const std::vector<std::string>& contents)
{
    std::lock_guard<std::mutex> lock(mu_);
    items_.clear();
    items_.reserve(contents.size());
    for (const auto& c : contents) {
        items_.push_back({c, "pending", ""});
    }
}

bool SessionTodoList::Complete(int index, const std::string& result)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (index < 0 || static_cast<size_t>(index) >= items_.size()) {
        return false;
    }
    items_[index].status = "completed";
    items_[index].result = result;
    return true;
}

bool SessionTodoList::Insert(int index, const std::string& content)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (index < 0) {
        return false;
    }
    if (static_cast<size_t>(index) > items_.size()) {
        index = static_cast<int>(items_.size());
    }
    items_.insert(items_.begin() + index, {content, "pending", ""});
    return true;
}

bool SessionTodoList::Remove(int index)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (index < 0 || static_cast<size_t>(index) >= items_.size()) {
        return false;
    }
    items_.erase(items_.begin() + index);
    return true;
}

std::vector<TodoItem> SessionTodoList::List() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return items_;
}

bool SessionTodoList::Empty() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return items_.empty();
}

std::string SessionTodoList::Render() const
{
    std::lock_guard<std::mutex> lock(mu_);
    if (items_.empty()) {
        return "";
    }
    std::ostringstream oss;
    oss << "# Current Todo List\n";
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& it = items_[i];
        oss << "  [" << i << "] (" << it.status << ") " << it.content;
        if (!it.result.empty()) {
            oss << " -> " << it.result;
        }
        oss << "\n";
    }
    return oss.str();
}

} // namespace jiuwen

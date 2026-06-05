#include "src/tools/builtin_tools/todo_tool.h"

#include <string>
#include <vector>

#include "src/core/session_todo_list.h"
#include "third_party/include/nlohmann/json.hpp"

namespace jiuwen {

namespace {

std::string MakeError(const std::string& msg)
{
    nlohmann::json j;
    j["ok"] = false;
    j["error"] = msg;
    return j.dump();
}

std::string MakeOk(const nlohmann::json& extra = nlohmann::json::object())
{
    nlohmann::json j = extra;
    j["ok"] = true;
    return j.dump();
}

nlohmann::json RenderItems(const std::vector<TodoItem>& items)
{
    nlohmann::json arr = nlohmann::json::array();
    for (size_t i = 0; i < items.size(); ++i) {
        nlohmann::json o;
        o["index"] = static_cast<int>(i);
        o["content"] = items[i].content;
        o["status"] = items[i].status;
        if (!items[i].result.empty()) {
            o["result"] = items[i].result;
        }
        arr.push_back(o);
    }
    return arr;
}

bool RequireList(SessionTodoList* list, std::string& errOut)
{
    if (list == nullptr) {
        errOut = MakeError("todo list is not available in this context");
        return false;
    }
    return true;
}

} // namespace

// ---------------- TodoCreateTool ----------------

TodoCreateTool::TodoCreateTool(SessionTodoList* list)
    : Tool("todo_create",
           "Create the initial todo list of tasks for the current run. "
           "Replaces any existing list. Input: JSON {\"items\": [\"task 1\", \"task 2\", ...]}.",
           {{"items", "An array of task descriptions (strings)", "array", true}}),
      list_(list) {}

std::string TodoCreateTool::Invoke(const std::string& input)
{
    std::string err;
    if (!RequireList(list_, err)) {
        return err;
    }
    std::vector<std::string> contents;
    try {
        auto j = nlohmann::json::parse(input);
        if (!j.contains("items") || !j["items"].is_array()) {
            return MakeError("'items' must be an array of strings");
        }
        for (const auto& v : j["items"]) {
            if (v.is_string()) {
                contents.push_back(v.get<std::string>());
            }
        }
    } catch (const std::exception& e) {
        return MakeError(std::string("invalid JSON: ") + e.what());
    }
    list_->Create(contents);
    nlohmann::json extra;
    extra["count"] = static_cast<int>(contents.size());
    extra["items"] = RenderItems(list_->List());
    return MakeOk(extra);
}

// ---------------- TodoCompleteTool ----------------

TodoCompleteTool::TodoCompleteTool(SessionTodoList* list)
    : Tool("todo_complete",
           "Mark a task as completed by its index. Optionally include a short result. "
           "Input: JSON {\"index\": <int>, \"result\": <string optional>}.",
           {{"index", "Zero-based task index", "integer", true},
            {"result", "Short result/note", "string", false}}),
      list_(list) {}

std::string TodoCompleteTool::Invoke(const std::string& input)
{
    std::string err;
    if (!RequireList(list_, err)) {
        return err;
    }
    int index = -1;
    std::string result;
    try {
        auto j = nlohmann::json::parse(input);
        if (!j.contains("index") || !j["index"].is_number_integer()) {
            return MakeError("'index' (integer) is required");
        }
        index = j["index"].get<int>();
        if (j.contains("result") && j["result"].is_string()) {
            result = j["result"].get<std::string>();
        }
    } catch (const std::exception& e) {
        return MakeError(std::string("invalid JSON: ") + e.what());
    }
    if (!list_->Complete(index, result)) {
        return MakeError("index out of range: " + std::to_string(index));
    }
    nlohmann::json extra;
    extra["items"] = RenderItems(list_->List());
    return MakeOk(extra);
}

// ---------------- TodoInsertTool ----------------

TodoInsertTool::TodoInsertTool(SessionTodoList* list)
    : Tool("todo_insert",
           "Insert a new task at a given index. Existing tasks shift down. "
           "Input: JSON {\"index\": <int>, \"content\": <string>}.",
           {{"index", "Zero-based insertion index (use list length to append)", "integer", true},
            {"content", "Task description", "string", true}}),
      list_(list) {}

std::string TodoInsertTool::Invoke(const std::string& input)
{
    std::string err;
    if (!RequireList(list_, err)) {
        return err;
    }
    int index = 0;
    std::string content;
    try {
        auto j = nlohmann::json::parse(input);
        if (!j.contains("index") || !j["index"].is_number_integer()) {
            return MakeError("'index' (integer) is required");
        }
        index = j["index"].get<int>();
        if (!j.contains("content") || !j["content"].is_string()) {
            return MakeError("'content' (string) is required");
        }
        content = j["content"].get<std::string>();
    } catch (const std::exception& e) {
        return MakeError(std::string("invalid JSON: ") + e.what());
    }
    if (!list_->Insert(index, content)) {
        return MakeError("insertion failed at index " + std::to_string(index));
    }
    nlohmann::json extra;
    extra["items"] = RenderItems(list_->List());
    return MakeOk(extra);
}

// ---------------- TodoRemoveTool ----------------

TodoRemoveTool::TodoRemoveTool(SessionTodoList* list)
    : Tool("todo_remove",
           "Remove a task by its index. Remaining tasks renumber. "
           "Input: JSON {\"index\": <int>}.",
           {{"index", "Zero-based task index", "integer", true}}),
      list_(list) {}

std::string TodoRemoveTool::Invoke(const std::string& input)
{
    std::string err;
    if (!RequireList(list_, err)) {
        return err;
    }
    int index = -1;
    try {
        auto j = nlohmann::json::parse(input);
        if (!j.contains("index") || !j["index"].is_number_integer()) {
            return MakeError("'index' (integer) is required");
        }
        index = j["index"].get<int>();
    } catch (const std::exception& e) {
        return MakeError(std::string("invalid JSON: ") + e.what());
    }
    if (!list_->Remove(index)) {
        return MakeError("index out of range: " + std::to_string(index));
    }
    nlohmann::json extra;
    extra["items"] = RenderItems(list_->List());
    return MakeOk(extra);
}

// ---------------- TodoListTool ----------------

TodoListTool::TodoListTool(SessionTodoList* list)
    : Tool("todo_list",
           "List all current todos with status and result. Input: empty JSON {} accepted.",
           {}),
      list_(list) {}

std::string TodoListTool::Invoke(const std::string& /*input*/)
{
    std::string err;
    if (!RequireList(list_, err)) {
        return err;
    }
    nlohmann::json extra;
    extra["items"] = RenderItems(list_->List());
    return MakeOk(extra);
}

} // namespace jiuwen

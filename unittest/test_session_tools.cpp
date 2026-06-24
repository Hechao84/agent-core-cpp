 
#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "include/resource_manager.h"
#include "src/core/ask_user_dispatcher.h"
#include "src/core/session_todo_list.h"
#include "third_party/include/nlohmann/json.hpp"
#include "test_runner.h"

using namespace jiuwen;

TEST(session_tools, TodoCreateRegistered)
{
    auto& rm = ResourceManager::GetInstance();
    TestRunner::AssertTrue(rm.HasSessionTool("todo_create"), "todo_create registered");
}

TEST(session_tools, TodoListRegistered)
{
    auto& rm = ResourceManager::GetInstance();
    TestRunner::AssertTrue(rm.HasSessionTool("todo_list"), "todo_list registered");
}

TEST(session_tools, AskUserRegistered)
{
    auto& rm = ResourceManager::GetInstance();
    TestRunner::AssertTrue(rm.HasSessionTool("ask_user"), "ask_user registered");
}

TEST(session_tools, RegularToolNotSessionTool)
{
    auto& rm = ResourceManager::GetInstance();
    TestRunner::AssertFalse(rm.HasSessionTool("time_info"), "time_info is not a session tool");
    TestRunner::AssertTrue(rm.HasTool("time_info"), "regular tool time_info still works");
}

TEST(session_tools, GetAvailableToolsMergesSessionTools)
{
    auto& rm = ResourceManager::GetInstance();
    auto names = rm.GetAvailableTools();
    bool foundTodo = false;
    bool foundAsk = false;
    for (const auto& n : names) {
        if (n == "todo_create") foundTodo = true;
        if (n == "ask_user") foundAsk = true;
    }
    TestRunner::AssertTrue(foundTodo && foundAsk, "GetAvailableTools() merges session tools");
}

TEST(session_tools, TodoCreatePopulatesItems)
{
    SessionTodoList list;
    ToolBuildContext ctx;
    ctx.todoList = &list;
    ctx.sessionId = "test-session";

    auto& rm = ResourceManager::GetInstance();
    auto create = rm.CreateSessionTool("todo_create", ctx);
    nlohmann::json args;
    args["items"] = {"task A", "task B", "task C"};
    std::string r1 = create->Invoke(args.dump());
    auto j1 = nlohmann::json::parse(r1);
    TestRunner::AssertTrue(j1["ok"].get<bool>(), "todo_create returns ok");
    TestRunner::AssertEq(j1["count"].get<int>(), 3, "todo_create populates 3 items");
}

TEST(session_tools, TodoCompleteMarksItem)
{
    SessionTodoList list;
    ToolBuildContext ctx;
    ctx.todoList = &list;
    ctx.sessionId = "test-session";

    auto& rm = ResourceManager::GetInstance();
    auto create = rm.CreateSessionTool("todo_create", ctx);
    nlohmann::json args;
    args["items"] = {"task A", "task B", "task C"};
    create->Invoke(args.dump());

    auto complete = rm.CreateSessionTool("todo_complete", ctx);
    nlohmann::json cargs;
    cargs["index"] = 1;
    cargs["result"] = "done!";
    auto r2 = nlohmann::json::parse(complete->Invoke(cargs.dump()));
    TestRunner::AssertTrue(r2["ok"].get<bool>(), "todo_complete returns ok");
    TestRunner::AssertEq(r2["items"][1]["status"].get<std::string>(), std::string("completed"),
                         "item 1 is completed");
}

TEST(session_tools, TodoListReturnsAllItems)
{
    SessionTodoList list;
    ToolBuildContext ctx;
    ctx.todoList = &list;
    ctx.sessionId = "test-session";

    auto& rm = ResourceManager::GetInstance();
    auto create = rm.CreateSessionTool("todo_create", ctx);
    nlohmann::json args;
    args["items"] = {"task A", "task B", "task C"};
    create->Invoke(args.dump());

    auto listTool = rm.CreateSessionTool("todo_list", ctx);
    auto r3 = nlohmann::json::parse(listTool->Invoke("{}"));
    TestRunner::AssertTrue(r3["ok"].get<bool>(), "todo_list returns ok");
    TestRunner::AssertEq(r3["items"].size(), size_t(3), "todo_list returns 3 items");
}

TEST(session_tools, DispatcherRoundTrip)
{
    AskUserDispatcher dispatcher;
    std::string captured;
    auto stream = [&captured](const std::string& s) { captured += s; };

    std::thread provider([&dispatcher]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        dispatcher.ProvideResponse("req-1", "the answer");
    });

    dispatcher.EmitAskUser("req-1", R"({"question":"x","request_id":"req-1"})", stream);
    auto ans = dispatcher.WaitForResponse("req-1", std::chrono::seconds(2));
    provider.join();

    TestRunner::AssertTrue(ans.has_value(), "dispatcher returns value");
    TestRunner::AssertEq(*ans, std::string("the answer"), "dispatcher returns correct answer");
    TestRunner::AssertContains(captured, "[ASK_USER]", "dispatcher emits [ASK_USER] tag");
}

TEST(session_tools, DispatcherTimeout)
{
    AskUserDispatcher dispatcher;
    auto missing = dispatcher.WaitForResponse("req-never", std::chrono::seconds(1));
    TestRunner::AssertFalse(missing.has_value(), "dispatcher returns nullopt on timeout");
}

TEST(session_tools, DispatcherWithRouterRegistersRequestId)
{
    class TestRouter : public AskUserRouter {
    public:
        void RegisterAskRequest(const std::string& requestId, const std::string& sessionId) override
        {
            registry[requestId] = sessionId;
        }
        void UnregisterAskRequest(const std::string& requestId) override
        {
            registry.erase(requestId);
        }
        std::unordered_map<std::string, std::string> registry;
    };

    TestRouter router;
    AskUserDispatcher dispatcher("sess-42", &router);
    std::string captured;
    auto stream = [&captured](const std::string& s) { captured += s; };

    std::thread provider([&dispatcher]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        dispatcher.ProvideResponse("req-r1", "answer-r1");
    });

    dispatcher.EmitAskUser("req-r1", R"({"question":"x","request_id":"req-r1"})", stream);
    TestRunner::AssertTrue(router.registry.count("req-r1") > 0, "requestId registered in router");
    TestRunner::AssertEq(router.registry["req-r1"], std::string("sess-42"), "correct sessionId in router");

    auto ans = dispatcher.WaitForResponse("req-r1", std::chrono::seconds(2));
    provider.join();

    TestRunner::AssertTrue(ans.has_value(), "dispatcher returns value with router");
    TestRunner::AssertEq(*ans, std::string("answer-r1"), "dispatcher returns correct answer");
    TestRunner::AssertTrue(router.registry.count("req-r1") == 0, "requestId unregistered after completion");
}

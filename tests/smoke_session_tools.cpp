// Standalone smoke test for the new session-tool registry + Todo + AskUser.
// Verifies (without booting the full Agent / model stack):
//   1. ResourceManager exposes todo_* and ask_user via session-tool API
//    2. GetAvailableTools() includes both regular and session tools
//   3. todo_create / todo_list round-trips through a freshly built tool
//   4. AskUserDispatcher Wait/Provide round-trip on its own
//   5. AskUserTool 60s timeout returns empty string when no provider responds
//      (we shrink the timeout by injecting via dispatcher directly so the
//       test runs fast)

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "include/resource_manager.h"
#include "src/core/ask_user_dispatcher.h"
#include "src/core/session_todo_list.h"
#include "third_party/include/nlohmann/json.hpp"

using namespace jiuwen;

static void Check(bool cond, const std::string& tag)
{
    if (!cond) {
        std::cerr << "[FAIL] " << tag << std::endl;
        std::exit(1);
    }
    std::cout << "[OK]   " << tag << std::endl;
}

int main()
{
    auto& rm = ResourceManager::GetInstance();

    // 1. Registry exposure
    Check(rm.HasSessionTool("todo_create"), "session tool todo_create registered");
    Check(rm.HasSessionTool("todo_list"), "session tool todo_list registered");
    Check(rm.HasSessionTool("ask_user"), "session tool ask_user registered");
    Check(!rm.HasSessionTool("time_info"), "time_info is not a session tool");
    Check(rm.HasTool("time_info"), "regular tool time_info still works");

    // 2. Unified availability list
    auto names = rm.GetAvailableTools();
    bool foundTodo = false;
    bool foundAsk = false;
    for (const auto& n : names) {
        if (n == "todo_create") foundTodo = true;
        if (n == "ask_user") foundAsk = true;
    }
    Check(foundTodo && foundAsk, "GetAvailableTools() merges session tools");

    // 3. Todo round-trip via a real per-session list
    SessionTodoList list;
    ToolBuildContext ctx;
    ctx.todoList = &list;
    ctx.sessionId = "smoke-session";

    auto create = rm.CreateSessionTool("todo_create", ctx);
    nlohmann::json args;
    args["items"] = {"task A", "task B", "task C"};
    std::string r1 = create->Invoke(args.dump());
    auto j1 = nlohmann::json::parse(r1);
    Check(j1["ok"].get<bool>() && j1["count"].get<int>() == 3, "todo_create populates 3 items");

    auto complete = rm.CreateSessionTool("todo_complete", ctx);
    nlohmann::json cargs;
    cargs["index"] = 1;
    cargs["result"] = "done!";
    auto r2 = nlohmann::json::parse(complete->Invoke(cargs.dump()));
    Check(r2["ok"].get<bool>(), "todo_complete returns ok");
    Check(r2["items"][1]["status"].get<std::string>() == "completed", "item 1 is completed");

    auto listTool = rm.CreateSessionTool("todo_list", ctx);
    auto r3 = nlohmann::json::parse(listTool->Invoke("{}"));
    Check(r3["ok"].get<bool>() && r3["items"].size() == 3, "todo_list returns 3 items");

    // 4. Dispatcher round-trip
    AskUserDispatcher dispatcher;
    std::string captured;
    auto stream = [&captured](const std::string& s) { captured += s; };
    std::thread provider([&dispatcher]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        bool ok = dispatcher.ProvideResponse("req-1", "the answer");
        if (!ok) std::cerr << "[WARN] ProvideResponse missed slot\n";
    });
    dispatcher.EmitAskUser("req-1", R"({"question":"x","request_id":"req-1"})", stream);
    auto ans = dispatcher.WaitForResponse("req-1", std::chrono::seconds(2));
    provider.join();
    Check(ans.has_value() && *ans == "the answer", "dispatcher wakes WaitForResponse");
    Check(captured.find("[ASK_USER]") != std::string::npos, "dispatcher emitted [ASK_USER] tag");

    // 5. Timeout path (use 1s by going through dispatcher directly)
    auto missing = dispatcher.WaitForResponse("req-never", std::chrono::seconds(1));
    Check(!missing.has_value(), "dispatcher returns nullopt on timeout");

    std::cout << "\nAll smoke checks passed.\n";
    return 0;
}

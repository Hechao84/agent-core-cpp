// Verify ContextEngine + JsonStorage round-trip structured Messages:
//   1. Write user / assistant(tool_calls) / tool / assistant(text)
//   2. Reload from disk -> exact field reconstruction
//   3. Orphan trailing assistant(tool_calls) is trimmed on load with warn

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "include/model.h"
#include "include/types.h"
#include "src/context_engine/context_engine.h"

namespace fs = std::filesystem;
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
    fs::path tmpdir = fs::temp_directory_path() / "jiuwen_smoke_ctx";
    fs::remove_all(tmpdir);
    fs::create_directories(tmpdir);

    ContextConfig cfg;
    cfg.sessionId = "sess-x";
    cfg.storagePath = tmpdir.string();
    cfg.storageType = ContextConfig::StorageType::JSON_FILE;
    cfg.maxContextTokens = 1024 * 10;
    cfg.maxMessages = 100;

    // ---- pass 1: write ----
    {
        ContextEngine ce(cfg);
        Check(ce.Initialize(), "init");

        Message u; u.role = "user"; u.content = "hi";
        ce.AddMessage(u);

        Message a; a.role = "assistant"; a.content = "thinking...";
        ToolCall tc; tc.id = "call_1"; tc.name = "echo"; tc.argumentsJson = "{\"x\":1}";
        a.toolCalls.push_back(tc);
        ce.AddMessage(a);

        Message t; t.role = "tool"; t.toolCallId = "call_1"; t.toolName = "echo"; t.content = "the result";
        ce.AddMessage(t);

        Message a2; a2.role = "assistant"; a2.content = "final answer";
        ce.AddMessage(a2);
    }

    // ---- pass 2: reload ----
    {
        ContextEngine ce(cfg);
        Check(ce.Initialize(), "reload init");
        auto msgs = ce.GetAllMessages();
        Check(msgs.size() == 4, "reload yields 4 messages, got " + std::to_string(msgs.size()));
        Check(msgs[0].role == "user" && msgs[0].content == "hi", "user msg ok");
        Check(msgs[1].role == "assistant" && msgs[1].toolCalls.size() == 1, "assistant tool_calls preserved");
        Check(msgs[1].toolCalls[0].id == "call_1", "tool_call id preserved");
        Check(msgs[1].toolCalls[0].name == "echo", "tool_call name preserved");
        Check(msgs[1].toolCalls[0].argumentsJson.find("\"x\":1") != std::string::npos, "tool_call args preserved");
        Check(msgs[1].content == "thinking...", "assistant text preserved");
        Check(msgs[2].role == "tool" && msgs[2].toolCallId == "call_1", "tool_call_id preserved");
        Check(msgs[2].content == "the result", "tool content preserved");
        Check(msgs[3].role == "assistant" && msgs[3].content == "final answer", "final assistant preserved");
    }

    // ---- pass 3: write an orphan trailing assistant(tool_calls), reload -> trimmed ----
    {
        ContextConfig cfg2 = cfg;
        cfg2.sessionId = "sess-orphan";
        fs::path dir2 = tmpdir / "orphan";
        fs::create_directories(dir2);
        cfg2.storagePath = dir2.string();
        {
            ContextEngine ce(cfg2);
            Check(ce.Initialize(), "orphan init");
            Message u; u.role = "user"; u.content = "do it";
            ce.AddMessage(u);
            Message a; a.role = "assistant"; a.content = "";
            ToolCall tc; tc.id = "call_orphan"; tc.name = "noop"; tc.argumentsJson = "{}";
            a.toolCalls.push_back(tc);
            ce.AddMessage(a);
            // No tool reply -- simulating an interrupted run.
        }
        ContextEngine ce(cfg2);
        Check(ce.Initialize(), "orphan reload");
        auto msgs = ce.GetAllMessages();
        Check(msgs.size() == 1, "orphan trailing assistant trimmed -> 1 message, got " + std::to_string(msgs.size()));
        Check(msgs[0].role == "user", "only user remains");
    }

    fs::remove_all(tmpdir);
    std::cout << "\nAll context_persist checks passed.\n";
    return 0;
}

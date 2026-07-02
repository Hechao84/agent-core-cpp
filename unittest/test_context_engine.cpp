

#include <filesystem>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "include/types.h"
#include "src/context_engine/context_engine.h"
#include "src/context_engine/db_storage.h"
#include "src/context_engine/json_storage.h"
#include "test_runner.h"

using namespace jiuwen;

namespace fs = std::filesystem;

TEST(context_engine, MemoryOnlyInit)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.sessionId = "test_mem";
    ContextEngine engine(config);
    bool result = engine.Initialize();
    TestRunner::AssertTrue(result);
    TestRunner::AssertEq(engine.GetSessionId(), std::string("test_mem"));
}

TEST(context_engine, AddAndRetrieveMessage)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.maxContextTokens = 10000;
    ContextEngine engine(config);
    engine.Initialize();
    engine.AddMessage({"user", "Hello"});
    engine.AddMessage({"assistant", "Hi there!"});
    auto messages = engine.GetContextWindow();
    TestRunner::AssertEq(messages.size(), size_t(2));
    TestRunner::AssertEq(messages[0].role, std::string("user"));
    TestRunner::AssertEq(messages[0].content, std::string("Hello"));
    TestRunner::AssertEq(messages[1].role, std::string("assistant"));
}

TEST(context_engine, Clear)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    ContextEngine engine(config);
    engine.Initialize();
    engine.AddMessage({"user", "test"});
    TestRunner::AssertEq(engine.GetTokenCount() > 0, true);
    engine.Clear();
    auto messages = engine.GetContextWindow();
    TestRunner::AssertEq(messages.size(), size_t(0));
}

TEST(context_engine, TokenBoundedWindow)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.maxContextTokens = 20; // ~80 chars (EstimateTokens = len/4)
    ContextEngine engine(config);
    engine.Initialize();
    engine.AddMessage({"user", "short"}); // 5 chars ~ 1 token
    engine.AddMessage({"user", "A much longer message that will exceed the token budget here"});
    auto messages = engine.GetContextWindow();
    // Should fit at least the short message
    TestRunner::AssertTrue(messages.size() >= 1);
}

TEST(context_engine, GetContextAsString)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.maxContextTokens = 10000;
    ContextEngine engine(config);
    engine.Initialize();
    engine.AddMessage({"user", "Hello"});
    std::string ctx = engine.GetContextAsString();
    TestRunner::AssertContains(ctx, "user: Hello");
}

TEST(context_engine, GetAllMessages)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    ContextEngine engine(config);
    engine.Initialize();
    engine.AddMessage({"user", "1"});
    engine.AddMessage({"assistant", "2"});
    engine.AddMessage({"user", "3"});
    auto all = engine.GetAllMessages();
    TestRunner::AssertEq(all.size(), size_t(3));
}

TEST(context_engine, MemoryOnlyStorage)
{
    ContextConfig config;
    config.sessionId = "mem_test";
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    ContextEngine engine(config);
    engine.Initialize();
    engine.AddMessage({"user", "persistent hello"});
    TestRunner::AssertEq(engine.GetTokenCount() > 0, true);
}

// JsonStorage Tests
TEST(json_storage, SaveAndLoad)
{
    std::string testDir = "test_tmp_json_storage";
    if (fs::exists(testDir)) fs::remove_all(testDir);
    fs::create_directories(testDir);

    JsonStorage storage(testDir, "test_session");
    storage.SaveMessage({"user", "Hello world"});
    storage.SaveMessage({"assistant", "Hi!"});

    std::vector<Message> loaded;
    bool ok = storage.LoadHistory(loaded);
    TestRunner::AssertTrue(ok);
    TestRunner::AssertEq(loaded.size(), size_t(2));
    TestRunner::AssertEq(loaded[0].content, std::string("Hello world"));
    TestRunner::AssertEq(loaded[1].content, std::string("Hi!"));

    fs::remove_all(testDir);
}

TEST(json_storage, PayloadRef)
{
    std::string testDir = "test_tmp_json_payload_ref";
    if (fs::exists(testDir)) fs::remove_all(testDir);
    fs::create_directories(testDir);

    JsonStorage storage(testDir, "session_payload");
    Message message;
    message.role = "tool";
    message.content = "[memory-ref: file://payload]";
    message.toolCallId = "call_1";
    message.toolName = "grep";
    message.payloadRef = "file://payload";
    storage.SaveMessage(message);

    std::vector<Message> loaded;
    storage.LoadHistory(loaded);
    TestRunner::AssertEq(loaded.size(), size_t(1));
    TestRunner::AssertEq(loaded[0].payloadRef, std::string("file://payload"));

    fs::remove_all(testDir);
}

TEST(json_storage, MultiLineContent)
{
    std::string testDir = "test_tmp_json_multiline";
    if (fs::exists(testDir)) fs::remove_all(testDir);
    fs::create_directories(testDir);

    JsonStorage storage(testDir, "session2");
    storage.SaveMessage({"assistant", "Line 1\nLine 2\nLine 3"});

    std::vector<Message> loaded;
    storage.LoadHistory(loaded);
    TestRunner::AssertEq(loaded.size(), size_t(1));
    TestRunner::AssertEq(loaded[0].role, std::string("assistant"));

    fs::remove_all(testDir);
}

TEST(json_storage, Clear)
{
    std::string testDir = "test_tmp_json_clear";
    if (fs::exists(testDir)) fs::remove_all(testDir);
    fs::create_directories(testDir);

    JsonStorage storage(testDir, "clear_session");
    storage.SaveMessage({"user", "test"});
    storage.Clear();

    std::vector<Message> loaded;
    storage.LoadHistory(loaded);
    TestRunner::AssertEq(loaded.size(), size_t(0));

    fs::remove_all(testDir);
}

TEST(json_storage, NonExistentFile)
{
    std::string testDir = "test_tmp_json_nofile";
    if (fs::exists(testDir)) fs::remove_all(testDir);
    fs::create_directories(testDir);

    JsonStorage storage(testDir, "empty_session");
    std::vector<Message> loaded;
    bool ok = storage.LoadHistory(loaded);
    TestRunner::AssertTrue(ok, "Should succeed (empty is OK)");
    TestRunner::AssertEq(loaded.size(), size_t(0));

    fs::remove_all(testDir);
}

TEST(context_engine, MaxMessagesLimit)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.maxMessages = 5;
    config.maxContextTokens = 100000; // High token limit to test message limit
    ContextEngine engine(config);
    engine.Initialize();
    
    // Add 10 messages with alternating roles to prevent sanitization merge
    for (int i = 0; i < 10; ++i) {
        std::string role = (i % 2 == 0) ? "user" : "assistant";
        engine.AddMessage({role, "Message " + std::to_string(i)});
    }
    
    auto window = engine.GetContextWindow();
    TestRunner::AssertTrue(window.size() <= static_cast<size_t>(5));
    TestRunner::AssertEq(window.back().content, std::string("Message 9"));
}

TEST(context_engine, TokenLimitWithMessageLimit)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.maxMessages = 10;
    config.maxContextTokens = 100; // ~400 chars
    ContextEngine engine(config);
    engine.Initialize();
    
    // Add messages that will exceed token limit
    for (int i = 0; i < 15; ++i) {
        engine.AddMessage({"user", "This is a longer message number " + std::to_string(i) + " with more content"});
    }
    
    auto window = engine.GetContextWindow();
    TestRunner::AssertTrue(window.size() <= 10);
    TestRunner::AssertTrue(engine.GetTokenCount() > config.maxContextTokens); // Total stored exceeds limit
}

TEST(context_engine, RecentUserSegmentPreserved)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.maxMessages = 3;
    config.maxContextTokens = 50;
    ContextEngine engine(config);
    engine.Initialize();
    
    engine.AddMessage({"user", "Important first message"});
    engine.AddMessage({"assistant", "Response 1"});
    engine.AddMessage({"user", "Message 2"});
    engine.AddMessage({"assistant", "Response 2"});
    engine.AddMessage({"user", "Message 3"});
    
    auto window = engine.GetContextWindow();
    TestRunner::AssertTrue(!window.empty());
    TestRunner::AssertEq(window.back().content, std::string("Message 3"));
}

TEST(context_engine, BuildMessagesForLLM)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.sessionId = "test_build";
    config.maxContextTokens = 10000;
    ContextEngine engine(config);
    engine.Initialize();
    
    std::vector<Message> history;
    history.push_back({"user", "Hello"});
    history.push_back({"assistant", "Hi there!"});
    
    Message currentMsg = {"user", "How are you?"};
    
    auto messages = engine.BuildMessagesForLLM(
        "You are a test assistant.",
        history,
        currentMsg
    );
    
    TestRunner::AssertTrue(messages.size() >= 3);
    TestRunner::AssertEq(messages[0].role, std::string("system"));
    TestRunner::AssertContains(messages[0].content, "You are a test assistant.");
    
    // Last message should have merged runtime context
    TestRunner::AssertEq(messages.back().role, std::string("user"));
    TestRunner::AssertContains(messages.back().content, "How are you?");
}

TEST(context_engine, BuildMessagesMergesSameRole)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.sessionId = "test_merge";
    config.maxContextTokens = 10000;
    ContextEngine engine(config);
    engine.Initialize();
    
    std::vector<Message> history;
    history.push_back({"user", "First question"});
    
    // Current message has same role as last history message
    Message currentMsg = {"user", "Second question"};
    
    auto messages = engine.BuildMessagesForLLM("", history, currentMsg);
    
    // Should merge the two user messages
    bool found = false;
    for (const auto& msg : messages) {
        if (msg.role == "user") {
            TestRunner::AssertContains(msg.content, "First question");
            TestRunner::AssertContains(msg.content, "Second question");
            found = true;
        }
    }
    TestRunner::AssertTrue(found);
}

TEST(context_engine, BuildMessagesCorrectRoleOrder)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.sessionId = "test_runtime";
    ContextEngine engine(config);
    engine.Initialize();
    
    std::vector<Message> history;
    history.push_back({"assistant", "Response"});
    Message currentMsg = {"user", "Query"};
    
    auto messages = engine.BuildMessagesForLLM("System Prompt", history, currentMsg);
    
    // Verify correct order: system -> history -> current msg
    TestRunner::AssertEq(messages.size(), size_t(3));
    TestRunner::AssertEq(messages[0].role, std::string("system"));
    TestRunner::AssertEq(messages[1].role, std::string("assistant"));
    TestRunner::AssertEq(messages[2].role, std::string("user"));
    TestRunner::AssertContains(messages[2].content, "Query");
}

TEST(context_engine, DropsOrphanToolCallsFromWindow)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.sessionId = "test_orphan_tools";
    config.maxContextTokens = 10000;
    ContextEngine engine(config);
    engine.Initialize();

    Message user;
    user.role = "user";
    user.content = "Need data";
    engine.AddMessage(user);

    ToolCall tc;
    tc.id = "";
    tc.name = "web_search";
    tc.argumentsJson = "{}";
    Message assistant;
    assistant.role = "assistant";
    assistant.toolCalls.push_back(tc);
    engine.AddMessage(assistant);

    auto window = engine.GetContextWindow();
    for (const auto& msg : window) {
        TestRunner::AssertTrue(msg.toolCalls.empty());
        TestRunner::AssertTrue(msg.role != "tool");
    }
}

TEST(context_engine, KeepsPairedToolMessagesInWindow)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.sessionId = "test_paired_tools";
    config.maxContextTokens = 10000;
    ContextEngine engine(config);
    engine.Initialize();

    Message user;
    user.role = "user";
    user.content = "Need data";
    engine.AddMessage(user);

    ToolCall tc;
    tc.id = "call_1";
    tc.name = "web_search";
    tc.argumentsJson = "{}";
    Message assistant;
    assistant.role = "assistant";
    assistant.toolCalls.push_back(tc);
    engine.AddMessage(assistant);

    Message tool;
    tool.role = "tool";
    tool.toolCallId = "call_1";
    tool.toolName = "web_search";
    tool.content = "result";
    engine.AddMessage(tool);

    auto window = engine.GetContextWindow();
    bool hasToolCall = false;
    bool hasToolResult = false;
    for (const auto& msg : window) {
        if (msg.role == "assistant" && !msg.toolCalls.empty()) hasToolCall = true;
        if (msg.role == "tool" && msg.toolCallId == "call_1") hasToolResult = true;
    }
    TestRunner::AssertTrue(hasToolCall);
    TestRunner::AssertTrue(hasToolResult);
}

TEST(context_engine, CompressSegmentRetainsSummaryAtExtremeLowBudget)
{
    // At an extremely small tokenBudget, CompressSegment must not drop the
    // summary entirely (which would collapse the window to just the bare user
    // message, losing all tool/assistant context). The summary is truncated
    // but retained as an assistant marker.
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.sessionId = "test_compress_lowbudget";
    config.enableSummarization = true;
    config.maxContextTokens = 10;   // tiny budget -> forces summary truncation
    config.maxMessages = 2;          // segment > 2 msgs -> triggers compression
    ContextEngine engine(config);
    engine.Initialize();

    Message user;
    user.role = "user";
    user.content = "do search";
    engine.AddMessage(user);

    auto addToolPair = [&](const std::string& callId) {
        Message a;
        a.role = "assistant";
        ToolCall tc;
        tc.id = callId;
        tc.name = "search";
        tc.argumentsJson = "{}";
        a.toolCalls.push_back(tc);
        engine.AddMessage(a);

        Message t;
        t.role = "tool";
        t.toolCallId = callId;
        t.toolName = "search";
        t.content = std::string(200, 'x');  // ~50 tokens each
        engine.AddMessage(t);
    };
    addToolPair("call_0");
    addToolPair("call_1");

    auto window = engine.GetContextWindow();
    //保底: an assistant summary marker must survive (truncated, not dropped).
    // Without保底, [user, summary] over budget would pop_back the summary,
    // leaving only the bare user message -> no assistant message at all.
    bool hasAssistantSummary = false;
    for (const auto& m : window) {
        if (m.role == "assistant" && !m.content.empty()) {
            hasAssistantSummary = true;
            break;
        }
    }
    TestRunner::AssertTrue(hasAssistantSummary);
}

TEST(context_engine, GetConsolidationPayload)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.sessionId = "test_consolidation";
    ContextEngine engine(config);
    engine.Initialize();
    
    for (int i = 0; i < 5; ++i) {
        engine.AddMessage({"user", "Message " + std::to_string(i)});
    }
    
    std::string payload = engine.GetConsolidationPayload(3);
    TestRunner::AssertContains(payload, "Message 2");
    TestRunner::AssertContains(payload, "Message 3");
    TestRunner::AssertContains(payload, "Message 4");
    TestRunner::AssertFalse(payload.find("Message 0") != std::string::npos);
}

TEST(context_engine, MemoryContextProviderOverridesLegacyMemory)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    ContextEngine engine(config);
    engine.Initialize();
    engine.SetMemoryContextProvider([]() {
        return std::string("runtime memory");
    });

    TestRunner::AssertEq(engine.GetMemoryContent(), std::string("runtime memory"));
}

TEST(context_engine, AddMessageEmitsMemoryEvent)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.sessionId = "event_session";
    ContextEngine engine(config);
    engine.Initialize();

    MemoryEvent captured;
    bool called = false;
    engine.SetMemoryEventSink([&](const MemoryEvent& event) {
        captured = event;
        called = true;
    });

    Message message;
    message.role = "tool";
    message.content = "tool output";
    message.toolCallId = "call_1";
    message.toolName = "grep";
    message.payloadRef = "file://payload";
    engine.AddMessage(message);

    TestRunner::AssertTrue(called);
    TestRunner::AssertTrue(captured.type == MemoryEventType::MESSAGE_APPENDED);
    TestRunner::AssertEq(captured.sessionId, std::string("event_session"));
    TestRunner::AssertEq(captured.role, std::string("tool"));
    TestRunner::AssertEq(captured.content, std::string("tool output"));
    TestRunner::AssertEq(captured.toolCallId, std::string("call_1"));
    TestRunner::AssertEq(captured.toolName, std::string("grep"));
    TestRunner::AssertEq(captured.payloadRef, std::string("file://payload"));

    // AddMessage auto-fills an ISO 8601 UTC timestamp when the caller omits
    // it, and propagates it to the MemoryEvent for memory consolidation.
    TestRunner::AssertTrue(!captured.timestamp.empty());
    TestRunner::AssertTrue(captured.timestamp.back() == 'Z');
    TestRunner::AssertTrue(captured.timestamp.find('T') != std::string::npos);
}

TEST(context_engine, JsonFileRoundTripWithToolCalls)
{
    std::string testDir = "test_tmp_ctx_persist";
    if (fs::exists(testDir)) fs::remove_all(testDir);
    fs::create_directories(testDir);

    ContextConfig cfg;
    cfg.sessionId = "sess-x";
    cfg.storagePath = testDir;
    cfg.storageType = ContextConfig::StorageType::JSON_FILE;
    cfg.maxContextTokens = 1024 * 10;
    cfg.maxMessages = 100;

    {
        ContextEngine ce(cfg);
        TestRunner::AssertTrue(ce.Initialize(), "init");

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

    {
        ContextEngine ce(cfg);
        TestRunner::AssertTrue(ce.Initialize(), "reload init");
        auto msgs = ce.GetAllMessages();
        TestRunner::AssertEq(msgs.size(), size_t(4), "reload yields 4 messages");
        TestRunner::AssertEq(msgs[0].role, std::string("user"));
        TestRunner::AssertEq(msgs[0].content, std::string("hi"));
        TestRunner::AssertEq(msgs[1].role, std::string("assistant"));
        TestRunner::AssertEq(msgs[1].toolCalls.size(), size_t(1), "assistant tool_calls preserved");
        TestRunner::AssertEq(msgs[1].toolCalls[0].id, std::string("call_1"));
        TestRunner::AssertEq(msgs[1].toolCalls[0].name, std::string("echo"));
        TestRunner::AssertContains(msgs[1].toolCalls[0].argumentsJson, "\"x\":1");
        TestRunner::AssertEq(msgs[1].content, std::string("thinking..."));
        TestRunner::AssertEq(msgs[2].role, std::string("tool"));
        TestRunner::AssertEq(msgs[2].toolCallId, std::string("call_1"));
        TestRunner::AssertEq(msgs[2].content, std::string("the result"));
        TestRunner::AssertEq(msgs[3].role, std::string("assistant"));
        TestRunner::AssertEq(msgs[3].content, std::string("final answer"));
    }

    fs::remove_all(testDir);
}

TEST(context_engine, OrphanTrailingAssistantTrimmedOnReload)
{
    std::string testDir = "test_tmp_ctx_orphan";
    if (fs::exists(testDir)) fs::remove_all(testDir);
    fs::create_directories(testDir);

    ContextConfig cfg;
    cfg.sessionId = "sess-orphan";
    cfg.storagePath = testDir;
    cfg.storageType = ContextConfig::StorageType::JSON_FILE;
    cfg.maxContextTokens = 1024 * 10;
    cfg.maxMessages = 100;

    {
        ContextEngine ce(cfg);
        TestRunner::AssertTrue(ce.Initialize(), "orphan init");
        Message u; u.role = "user"; u.content = "do it";
        ce.AddMessage(u);
        Message a; a.role = "assistant"; a.content = "";
        ToolCall tc; tc.id = "call_orphan"; tc.name = "noop"; tc.argumentsJson = "{}";
        a.toolCalls.push_back(tc);
        ce.AddMessage(a);
    }

    ContextEngine ce(cfg);
    TestRunner::AssertTrue(ce.Initialize(), "orphan reload");
    auto msgs = ce.GetAllMessages();
    TestRunner::AssertEq(msgs.size(), size_t(1), "orphan trailing assistant trimmed -> 1 message");
    TestRunner::AssertEq(msgs[0].role, std::string("user"), "only user remains");

    fs::remove_all(testDir);
}

// CJK text must not be under-counted the way length/4 did. With the
// UTF-8-aware estimator, a window of many Chinese messages under a small token
// budget should be trimmed to far fewer than the message-count limit, because
// each Chinese character costs ~0.75 token rather than ~0.25.
TEST(context_engine, CjkTokenEstimationBoundsWindow)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.maxMessages = 50;
    config.maxContextTokens = 40; // small budget
    ContextEngine engine(config);
    engine.Initialize();

    // Each message: 20 Chinese chars (~15 tokens under the new estimator;
    // would be only ~5 under the old length/4 since each char is 3 bytes).
    const std::string zh = "这是一段用于测试的中文消息内容长度二十字"; // 20 CJK chars
    for (int i = 0; i < 10; ++i) {
        engine.AddMessage({"user", zh});
    }

    auto window = engine.GetContextWindow();
    // Under a 40-token budget with ~15 tokens/message, only a couple messages
    // can fit. Assert the limiter did not admit anywhere near all 10.
    TestRunner::AssertTrue(window.size() <= 4,
        "CJK window must be tightly bounded by token budget");
    TestRunner::AssertTrue(!window.empty(), "at least the latest message kept");
}

// The latest user query is always retained by GetContextWindow,
// even when many assistant/tool turns precede it and the budget is tight. This
// is what makes the worker's per-iteration GetContextWindow refresh safe.
TEST(context_engine, LatestUserQueryAlwaysRetained)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.maxMessages = 6;
    config.maxContextTokens = 60;
    ContextEngine engine(config);
    engine.Initialize();

    // Simulate a long tool-calling round: user query followed by many
    // assistant/tool turns that would overflow the budget.
    engine.AddMessage({"user", "PLEASE_REMEMBER_THIS_QUERY"});
    for (int i = 0; i < 8; ++i) {
        Message a; a.role = "assistant"; a.content = "";
        ToolCall tc; tc.id = "call_" + std::to_string(i); tc.name = "search";
        tc.argumentsJson = "{\"q\":\"some longer query text number " + std::to_string(i) + "\"}";
        a.toolCalls.push_back(tc);
        engine.AddMessage(a);
        Message t; t.role = "tool"; t.toolCallId = tc.id; t.toolName = "search";
        t.content = "a fairly long tool observation result body number " + std::to_string(i);
        engine.AddMessage(t);
    }

    auto window = engine.GetContextWindow();
    TestRunner::AssertTrue(!window.empty(), "window not empty");
    bool hasQuery = false;
    for (const auto& m : window) {
        if (m.role == "user" && m.content.find("PLEASE_REMEMBER_THIS_QUERY") != std::string::npos) {
            hasQuery = true;
            break;
        }
    }
    TestRunner::AssertTrue(hasQuery,
        "latest user query must survive context-window trimming/compression");
}

TEST(db_storage, MigratesV2SchemaToAddTimestampColumn)
{
    // A v2-era DB has the v2 columns (tool_calls etc.) but no timestamp
    // column. DbStorage must ALTER-add timestamp on open so SaveMessage /
    // LoadHistory do not fail with "no such column: timestamp".
    const std::string dbPath = "test_tmp_db_v2_migration.db";
    fs::remove(dbPath);

    // Build a v2-era schema directly via sqlite3 (no timestamp column).
    sqlite3* raw = nullptr;
    TestRunner::AssertTrue(sqlite3_open(dbPath.c_str(), &raw) == SQLITE_OK);
    const char* v2schema =
        "CREATE TABLE messages (id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "session_id TEXT NOT NULL, role TEXT NOT NULL, content TEXT, "
        "tool_calls TEXT, tool_call_id TEXT, tool_name TEXT, payload_ref TEXT);";
    TestRunner::AssertTrue(sqlite3_exec(raw, v2schema, nullptr, nullptr, nullptr) == SQLITE_OK);
    // Insert a legacy row (no timestamp value -- column does not exist yet).
    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(raw,
        "INSERT INTO messages (session_id, role, content) VALUES (?, ?, ?);",
        -1, &ins, nullptr);
    sqlite3_bind_text(ins, 1, "v2sess", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, "user", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 3, "legacy row", -1, SQLITE_TRANSIENT);
    sqlite3_step(ins);
    sqlite3_finalize(ins);
    sqlite3_close(raw);

    // Open via DbStorage -> InitDatabase/CreateTable runs the migration.
    DbStorage storage(dbPath, "v2sess");

    // SaveMessage must succeed (INSERT references timestamp column).
    Message m;
    m.role = "assistant";
    m.content = "after migration";
    m.timestamp = "2026-07-02T11:12:38Z";
    TestRunner::AssertTrue(storage.SaveMessage(m));

    // LoadHistory must succeed and return both the legacy row and the new one.
    std::vector<Message> loaded;
    TestRunner::AssertTrue(storage.LoadHistory(loaded));
    TestRunner::AssertTrue(loaded.size() >= 2);

    // The legacy row has no timestamp (NULL -> empty); the new one carries
    // the ISO 8601 timestamp we saved.
    bool foundNew = false;
    bool foundLegacy = false;
    for (const auto& msg : loaded) {
        if (msg.content == "after migration") {
            foundNew = true;
            TestRunner::AssertEq(msg.timestamp, std::string("2026-07-02T11:12:38Z"));
        }
        if (msg.content == "legacy row") {
            foundLegacy = true;
            TestRunner::AssertTrue(msg.timestamp.empty());
        }
    }
    TestRunner::AssertTrue(foundNew);
    TestRunner::AssertTrue(foundLegacy);

    fs::remove(dbPath);
}

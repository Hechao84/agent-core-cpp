

#include <string>
#include <vector>
#include "filesystem"
#include "include/types.h"
#include "src/context_engine/context_engine.h"
#include "src/context_engine/md_storage.h"
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

// MarkdownStorage Tests
TEST(markdown_storage, SaveAndLoad)
{
    std::string testDir = "test_tmp_md_storage";
    if (fs::exists(testDir)) fs::remove_all(testDir);
    fs::create_directories(testDir);

    MarkdownStorage storage(testDir, "test_session");
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

TEST(markdown_storage, MultiLineContent)
{
    std::string testDir = "test_tmp_md_multiline";
    if (fs::exists(testDir)) fs::remove_all(testDir);
    fs::create_directories(testDir);

    MarkdownStorage storage(testDir, "session2");
    storage.SaveMessage({"assistant", "Line 1\nLine 2\nLine 3"});

    std::vector<Message> loaded;
    storage.LoadHistory(loaded);
    TestRunner::AssertEq(loaded.size(), size_t(1));
    TestRunner::AssertEq(loaded[0].role, std::string("assistant"));

    fs::remove_all(testDir);
}

TEST(markdown_storage, Clear)
{
    std::string testDir = "test_tmp_md_clear";
    if (fs::exists(testDir)) fs::remove_all(testDir);
    fs::create_directories(testDir);

    MarkdownStorage storage(testDir, "clear_session");
    storage.SaveMessage({"user", "test"});
    storage.Clear();

    std::vector<Message> loaded;
    storage.LoadHistory(loaded);
    TestRunner::AssertEq(loaded.size(), size_t(0));

    fs::remove_all(testDir);
}

TEST(markdown_storage, NonExistentFile)
{
    std::string testDir = "test_tmp_md_nofile";
    if (fs::exists(testDir)) fs::remove_all(testDir);
    fs::create_directories(testDir);

    MarkdownStorage storage(testDir, "empty_session");
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
    
    // Add 10 messages
    for (int i = 0; i < 10; ++i) {
        engine.AddMessage({"user", "Message " + std::to_string(i)});
    }
    
    auto window = engine.GetContextWindow();
    // Should keep first message + 4 recent messages = 5 total
    TestRunner::AssertTrue(window.size() <= 5);
    TestRunner::AssertEq(window[0].content, std::string("Message 0")); // First message preserved
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

TEST(context_engine, FirstMessageAlwaysPreserved)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.maxMessages = 3;
    config.maxContextTokens = 50; // Very low token limit
    ContextEngine engine(config);
    engine.Initialize();
    
    engine.AddMessage({"user", "Important first message"});
    engine.AddMessage({"assistant", "Response 1"});
    engine.AddMessage({"user", "Message 2"});
    engine.AddMessage({"assistant", "Response 2"});
    engine.AddMessage({"user", "Message 3"});
    
    auto window = engine.GetContextWindow();
    TestRunner::AssertTrue(!window.empty());
    TestRunner::AssertEq(window[0].content, std::string("Important first message"));
}

TEST(context_engine, MemoryProvidesDataForPrompts)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.sessionId = "test_prompt_data";
    ContextEngine engine(config);
    engine.Initialize();
    
    // ContextEngine provides data, AgentWorker assembles the prompt
    engine.UpdateMemory("User prefers JSON output.");
    std::string mem = engine.GetMemoryContent();
    TestRunner::AssertContains(mem, "User prefers JSON output.");
    
    engine.ClearMemory();
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

TEST(context_engine, UpdateMemory)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.sessionId = "test_memory";
    ContextEngine engine(config);
    engine.Initialize();
    
    engine.UpdateMemory("User prefers short answers. Main language is Chinese.");
    
    // Verify memory was loaded
    std::string mem = engine.GetMemoryContent();
    TestRunner::AssertContains(mem, "User prefers short answers.");
    
    // Cleanup
    engine.ClearMemory();
}

TEST(context_engine, UpdateMemoryMultipleTimes)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.sessionId = "test_memory_multi";
    ContextEngine engine(config);
    engine.Initialize();
    
    engine.UpdateMemory("Fact 1: User is a developer.");
    engine.UpdateMemory("Fact 2: User works on AI projects.");
    
    std::string mem = engine.GetMemoryContent();
    TestRunner::AssertContains(mem, "User is a developer.");
    TestRunner::AssertContains(mem, "User works on AI projects.");
    TestRunner::AssertContains(mem, "---"); // Should have separator
    
    engine.ClearMemory();
}

TEST(context_engine, ClearMemory)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.sessionId = "test_memory_clear";
    ContextEngine engine(config);
    engine.Initialize();
    
    engine.UpdateMemory("Some fact to be cleared.");
    std::string mem = engine.GetMemoryContent();
    TestRunner::AssertTrue(!mem.empty());
    
    engine.ClearMemory();
    mem = engine.GetMemoryContent();
    TestRunner::AssertEq(mem, std::string(""));
}

TEST(context_engine, OverwriteMemory)
{
    ContextConfig config;
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY;
    config.sessionId = "test_memory_overwrite";
    ContextEngine engine(config);
    engine.Initialize();
    
    engine.UpdateMemory("Old fact 1.\nOld fact 2.");
    std::string mem = engine.GetMemoryContent();
    TestRunner::AssertContains(mem, "Old fact 1.");
    TestRunner::AssertContains(mem, "Old fact 2.");
    
    engine.OverwriteMemory("New fact A.\nNew fact B.");
    mem = engine.GetMemoryContent();
    TestRunner::AssertContains(mem, "New fact A.");
    TestRunner::AssertContains(mem, "New fact B.");
    TestRunner::AssertFalse(mem.find("Old fact") != std::string::npos);
    
    engine.ClearMemory();
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

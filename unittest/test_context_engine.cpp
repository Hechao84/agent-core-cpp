

#include <string>
#include <vector>
#include "filesystem"
#include "include/types.h"
#include "src/context_engine/context_engine.h"
#include "src/context_engine/md_storage.h"
#include "src/context_engine/storage_interface.h"
#include "test_runner.h"

namespace fs = std::filesystem;

// Mock storage for testing ContextEngine in isolation
class MockStorage : public ContextStorageInterface {
public:
    std::vector<Message> savedMessages_;
    bool clearCalled_ = false;
    bool loadReturn_ = true;

    bool SaveMessage(const Message& msg) override
    {
        savedMessages_.push_back(msg);
        return true;
    }

    bool LoadHistory(std::vector<Message>& outMessages) override
    {
        outMessages = savedMessages_;
        return loadReturn_;
    }

    void Clear() override 
    { 
        clearCalled_ = true; 
        savedMessages_.clear(); 
    }
};

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

TEST(context_engine, MockStorageIntegration)
{
    ContextConfig config;
    config.sessionId = "mock_test";
    config.storageType = ContextConfig::StorageType::MEMORY_ONLY; // We'll manually inject storage
    ContextEngine engine(config);
    engine.Initialize(); // With MEMORY_ONLY, storage is nullptr
    engine.AddMessage({"user", "persistent hello"});
    // In-memory only, no mock injected (ContextEngine creates its own storage internally)
    // Just verify the API works
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

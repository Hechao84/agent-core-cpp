#include "test_runner.h"
#include "tools/tool_selector.h"
#include <string>
#include <vector>

TEST(tool_selector, AddToPool)
{
    ToolSelector selector;
    selector.AddToolToPool("tool_a");
    selector.AddToolToPool("tool_b");
    auto pool = selector.GetToolPool();
    TestRunner::AssertEq(pool.size(), size_t(2));
}

TEST(tool_selector, NoDuplicates)
{
    ToolSelector selector;
    selector.AddToolToPool("tool_a");
    selector.AddToolToPool("tool_a");
    selector.AddToolToPool("tool_a");
    auto pool = selector.GetToolPool();
    TestRunner::AssertEq(pool.size(), size_t(1));
}

TEST(tool_selector, RemoveFromPool)
{
    ToolSelector selector;
    selector.AddToolToPool("tool_a");
    selector.AddToolToPool("tool_b");
    selector.RemoveToolFromPool("tool_a");
    auto pool = selector.GetToolPool();
    TestRunner::AssertEq(pool.size(), size_t(1));
    TestRunner::AssertEq(pool[0], std::string("tool_b"));
}

TEST(tool_selector, RemoveNonexistent)
{
    ToolSelector selector;
    selector.AddToolToPool("tool_a");
    selector.RemoveToolFromPool("nonexistent"); // Should not crash
    auto pool = selector.GetToolPool();
    TestRunner::AssertEq(pool.size(), size_t(1));
}

TEST(tool_selector, SelectTopTools)
{
    ToolSelector selector;
    std::vector<std::string> available = {"read_file", "write_file", "shell"};
    auto top = selector.SelectTopTools("read a file", available, 2);
    // All tools return score 1.0 (stub), so should return up to maxCount
    TestRunner::AssertTrue(top.size() <= 2, "Should return at most 2 tools");
}

TEST(tool_selector, SelectToolReturnsEmpty)
{
    ToolSelector selector;
    std::vector<std::string> empty;
    std::string result = selector.SelectTool("query", empty);
    TestRunner::AssertTrue(result.empty(), "Should return empty string for no tools");
}

TEST(tool_selector, RankTools)
{
    ToolSelector selector;
    std::vector<std::string> available = {"tool_a", "tool_b", "tool_c"};
    auto results = selector.RankTools("query", available);
    TestRunner::AssertEq(results.size(), size_t(3));
    // All scores should be equal (stub returns 1.0)
    TestRunner::AssertTrue(results[0].score > 0, "Score should be positive");
}

TEST(tool_selector, SetSearchConfig)
{
    ToolSelector selector;
    SearchConfig config;
    config.keywordWeight = 0.8;
    config.embeddingWeight = 0.2;
    selector.SetSearchConfig(config);
    // Should not crash; subsequent operations work
    std::vector<std::string> available = {"tool_a"};
    auto results = selector.RankTools("query", available);
    TestRunner::AssertTrue(!results.empty());
}

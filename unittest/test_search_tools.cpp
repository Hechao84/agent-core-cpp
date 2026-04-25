

#include <fstream>
#include <string>
#include "filesystem"
#include "src/tools/builtin_tools/glob_tool.h"
#include "src/tools/builtin_tools/grep_tool.h"
#include "src/tools/builtin_tools/list_dir_tool.h"
#include "test_runner.h"

using namespace jiuwen;

namespace fs = std::filesystem;

static std::string GetTestDir() 
{ 
    return "test_tmp_listdir"; 
}

static void SetupTestDir()
{
    if (fs::exists(GetTestDir())) fs::remove_all(GetTestDir());
    fs::create_directories(GetTestDir() + "/sub1");
    fs::create_directories(GetTestDir() + "/sub2");
    fs::create_directories(GetTestDir() + "/.git");
    std::ofstream(GetTestDir() + "/a.txt") << "a";
    std::ofstream(GetTestDir() + "/b.cpp") << "b";
    std::ofstream(GetTestDir() + "/sub1/c.py") << "c";
    std::ofstream(GetTestDir() + "/sub1/d.cpp") << "d";
    std::ofstream(GetTestDir() + "/.git/config") << "x";
}

static void CleanupTestDir()
{
    if (fs::exists(GetTestDir())) fs::remove_all(GetTestDir());
}

// ListDirTool Tests
TEST(list_dir_tool, MissingPath)
{
    ListDirTool tool;
    std::string result = tool.Invoke("{\"recursive\":false}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "path");
}

TEST(list_dir_tool, PathNotFound)
{
    ListDirTool tool;
    std::string result = tool.Invoke("{\"path\":\"nonexistent_dir_12345\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "not found");
}

TEST(list_dir_tool, NotADir)
{
    ListDirTool tool;
    std::string result = tool.Invoke("{\"path\":\"CMakeLists.txt\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "Not a directory");
}

TEST(list_dir_tool, ListsDir)
{
    SetupTestDir();
    ListDirTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_listdir\"}");
    TestRunner::AssertContains(result, "a.txt");
    TestRunner::AssertContains(result, "b.cpp");
    TestRunner::AssertContains(result, "sub1");
    TestRunner::AssertContains(result, "sub2");
    // Should not list .git contents
    TestRunner::AssertFalse(result.find(".git/config") != std::string::npos, "Should skip .git files");
    CleanupTestDir();
}

TEST(list_dir_tool, Recursive)
{
    SetupTestDir();
    ListDirTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_listdir\",\"recursive\":true}");
    TestRunner::AssertContains(result, "a.txt");
    TestRunner::AssertContains(result, "sub1/");
    TestRunner::AssertContains(result, "sub1/c.py");
    TestRunner::AssertContains(result, "sub1/d.cpp");
    CleanupTestDir();
}

TEST(list_dir_tool, MaxEntries)
{
    SetupTestDir();
    ListDirTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_listdir\",\"recursive\":true,\"max_entries\":2}");
    TestRunner::AssertContains(result, "truncated");
    TestRunner::AssertContains(result, "showing first 2");
    CleanupTestDir();
}

// GlobTool Tests
TEST(glob_tool, MissingPattern)
{
    GlobTool tool;
    std::string result = tool.Invoke("{\"path\":\".\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "pattern");
}

TEST(glob_tool, MatchesFiles)
{
    SetupTestDir();
    GlobTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_listdir\",\"pattern\":\"*.cpp\"}");
    TestRunner::AssertContains(result, "b.cpp");
    TestRunner::AssertFalse(result.find("a.txt") != std::string::npos, "Should not match .txt");
    CleanupTestDir();
}

TEST(glob_tool, MatchesRecursivePattern)
{
    SetupTestDir();
    GlobTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_listdir\",\"pattern\":\"**/*.py\"}");
    TestRunner::AssertContains(result, "sub1/c.py");
    CleanupTestDir();
}

TEST(glob_tool, NoMatch)
{
    SetupTestDir();
    GlobTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_listdir\",\"pattern\":\"*.xyz\"}");
    TestRunner::AssertContains(result, "No paths matched");
    CleanupTestDir();
}

// GrepTool Tests
TEST(grep_tool, MissingPattern)
{
    GrepTool tool;
    std::string result = tool.Invoke("{\"path\":\".\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "pattern");
}

TEST(grep_tool, InvalidRegex)
{
    SetupTestDir();
    GrepTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_listdir\",\"pattern\":\"[invalid\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "invalid regex");
    CleanupTestDir();
}

TEST(grep_tool, FilesWithMatches)
{
    SetupTestDir();
    GrepTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_listdir\",\"pattern\":\"b\",\"output_mode\":\"files_with_matches\"}");
    TestRunner::AssertContains(result, "b.cpp");
    TestRunner::AssertFalse(result.find("a.txt") != std::string::npos, "a.txt should not match");
    CleanupTestDir();
}

TEST(grep_tool, ContentMode)
{
    SetupTestDir();
    GrepTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_listdir\",\"pattern\":\"b\",\"output_mode\":\"content\"}");
    TestRunner::AssertContains(result, "b.cpp");
    CleanupTestDir();
}

TEST(grep_tool, CountMode)
{
    SetupTestDir();
    GrepTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_listdir\",\"pattern\":\"b\",\"output_mode\":\"count\"}");
    TestRunner::AssertContains(result, "b.cpp:");
    CleanupTestDir();
}

TEST(grep_tool, NoMatches)
{
    SetupTestDir();
    GrepTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_listdir\",\"pattern\":\"zzzznotfound\"}");
    TestRunner::AssertContains(result, "No matches found");
    CleanupTestDir();
}

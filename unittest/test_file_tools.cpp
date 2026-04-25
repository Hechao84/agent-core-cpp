

#include <fstream>
#include <string>
#include "filesystem"
#include "src/tools/builtin_tools/edit_file_tool.h"
#include "src/tools/builtin_tools/read_file_tool.h"
#include "src/tools/builtin_tools/write_file_tool.h"
#include "test_runner.h"

using namespace jiuwen;

namespace fs = std::filesystem;

static std::string GetTestDir() 
{ 
    return "test_tmp_read_write"; 
}

static void SetupTestDir()
{
    if (fs::exists(GetTestDir())) fs::remove_all(GetTestDir());
    fs::create_directories(GetTestDir());
}

static void CleanupTestDir()
{
    if (fs::exists(GetTestDir())) fs::remove_all(GetTestDir());
}

// WriteFileTool Tests
TEST(write_file_tool, MissingPath)
{
    WriteFileTool tool;
    std::string result = tool.Invoke("{\"content\":\"test\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "path");
}

TEST(write_file_tool, WritesFile)
{
    SetupTestDir();
    WriteFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_read_write/hello.txt\",\"content\":\"Hello World\"}");
    TestRunner::AssertContains(result, "Successfully wrote");
    TestRunner::AssertEq(fs::exists("test_tmp_read_write/hello.txt"), true);
    std::ifstream f("test_tmp_read_write/hello.txt");
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    TestRunner::AssertEq(content, std::string("Hello World"));
    CleanupTestDir();
}

TEST(write_file_tool, CreatesParentDirs)
{
    SetupTestDir();
    WriteFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_read_write/sub/deep/file.txt\",\"content\":\"nested\"}");
    TestRunner::AssertContains(result, "Successfully wrote");
    TestRunner::AssertEq(fs::exists("test_tmp_read_write/sub/deep/file.txt"), true);
    CleanupTestDir();
}

TEST(write_file_tool, OverwritesExisting)
{
    SetupTestDir();
    std::ofstream f("test_tmp_read_write/existing.txt");
    f << "old content";
    f.close();
    WriteFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_read_write/existing.txt\",\"content\":\"new content\"}");
    TestRunner::AssertContains(result, "Successfully wrote");
    std::ifstream f2("test_tmp_read_write/existing.txt");
    std::string content((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
    TestRunner::AssertEq(content, std::string("new content"));
    CleanupTestDir();
}

// ReadFileTool Tests
TEST(read_file_tool, MissingPath)
{
    ReadFileTool tool;
    std::string result = tool.Invoke("{\"limit\":100}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "path");
}

TEST(read_file_tool, FileNotFound)
{
    ReadFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"nonexistent_file.txt\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "File not found");
}

TEST(read_file_tool, NotAFile)
{
    SetupTestDir();
    fs::create_directories("test_tmp_read_write/mydir");
    ReadFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_read_write/mydir\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "Not a file");
    CleanupTestDir();
}

TEST(read_file_tool, ReadsFile)
{
    SetupTestDir();
    std::ofstream f("test_tmp_read_write/readme.txt");
    f << "line1\nline2\nline3\n";
    f.close();
    ReadFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_read_write/readme.txt\"}");
    TestRunner::AssertContains(result, "1| line1");
    TestRunner::AssertContains(result, "2| line2");
    TestRunner::AssertContains(result, "3| line3");
    TestRunner::AssertContains(result, "End of file");
    TestRunner::AssertContains(result, "3 lines total");
    CleanupTestDir();
}

TEST(read_file_tool, EmptyFile)
{
    SetupTestDir();
    std::ofstream f("test_tmp_read_write/empty.txt");
    f.close();
    ReadFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_read_write/empty.txt\"}");
    TestRunner::AssertContains(result, "Empty file");
    CleanupTestDir();
}

TEST(read_file_tool, OffsetAndLimit)
{
    SetupTestDir();
    std::ofstream f("test_tmp_read_write/paginated.txt");
    for (int i = 1; i <= 10; ++i) f << "line" << i << "\n";
    f.close();
    ReadFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_read_write/paginated.txt\",\"offset\":3,\"limit\":4}");
    TestRunner::AssertContains(result, "3| line3");
    TestRunner::AssertContains(result, "6| line6");
    TestRunner::AssertFalse(result.find("7| line7") != std::string::npos, "Should not show line 7");
    TestRunner::AssertContains(result, "Showing lines 3-6");
    TestRunner::AssertContains(result, "Use offset=7");
    CleanupTestDir();
}

TEST(read_file_tool, OffsetBeyondEnd)
{
    SetupTestDir();
    std::ofstream f("test_tmp_read_write/short.txt");
    f << "only 3 lines\n";
    f.close();
    ReadFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_read_write/short.txt\",\"offset\":100}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "beyond end");
    CleanupTestDir();
}

// EditFileTool Tests
TEST(edit_file_tool, MissingPath)
{
    EditFileTool tool;
    std::string result = tool.Invoke("{\"old_text\":\"x\",\"new_text\":\"y\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "path");
}

TEST(edit_file_tool, FileNotFound)
{
    EditFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"nonexistent.txt\",\"old_text\":\"x\",\"new_text\":\"y\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "File not found");
}

TEST(edit_file_tool, OldTextNotFound)
{
    SetupTestDir();
    std::ofstream f("test_tmp_read_write/edit_test.txt");
    f << "hello world\n";
    f.close();
    EditFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_read_write/edit_test.txt\",\"old_text\":\"not present\",\"new_text\":\"replaced\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "old_text not found");
    CleanupTestDir();
}

TEST(edit_file_tool, SimpleReplace)
{
    SetupTestDir();
    std::ofstream f("test_tmp_read_write/edit_test2.txt");
    f << "hello world\n";
    f.close();
    EditFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_read_write/edit_test2.txt\",\"old_text\":\"hello world\",\"new_text\":\"goodbye world\"}");
    TestRunner::AssertContains(result, "Successfully edited");
    std::ifstream f2("test_tmp_read_write/edit_test2.txt");
    std::string content((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
    TestRunner::AssertEq(content, std::string("goodbye world\n"));
    CleanupTestDir();
}

TEST(edit_file_tool, ReplaceAll)
{
    SetupTestDir();
    std::ofstream f("test_tmp_read_write/edit_multi.txt");
    f << "foo bar foo baz foo\n";
    f.close();
    EditFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_read_write/edit_multi.txt\",\"old_text\":\"foo\",\"new_text\":\"qux\",\"replace_all\":true}");
    TestRunner::AssertContains(result, "Successfully edited");
    std::ifstream f2("test_tmp_read_write/edit_multi.txt");
    std::string content((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
    TestRunner::AssertEq(content, std::string("qux bar qux baz qux\n"));
    CleanupTestDir();
}

TEST(edit_file_tool, CreatesEmptyFile)
{
    SetupTestDir();
    EditFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_read_write/new_file.txt\",\"old_text\":\"\",\"new_text\":\"new content here\"}");
    TestRunner::AssertContains(result, "Successfully created");
    TestRunner::AssertEq(fs::exists("test_tmp_read_write/new_file.txt"), true);
    std::ifstream f("test_tmp_read_write/new_file.txt");
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    TestRunner::AssertEq(content, std::string("new content here"));
    CleanupTestDir();
}

TEST(edit_file_tool, RejectsNonEmptyFileCreation)
{
    SetupTestDir();
    std::ofstream f("test_tmp_read_write/not_empty.txt");
    f << "existing content";
    f.close();
    EditFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_read_write/not_empty.txt\",\"old_text\":\"\",\"new_text\":\"replacement\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "already exists and is not empty");
    CleanupTestDir();
}

TEST(edit_file_tool, RejectsIpynb)
{
    SetupTestDir();
    std::ofstream f("test_tmp_read_write/test.ipynb");
    f << "{}";
    f.close();
    EditFileTool tool;
    std::string result = tool.Invoke("{\"path\":\"test_tmp_read_write/test.ipynb\",\"old_text\":\"a\",\"new_text\":\"b\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "notebook_edit");
    CleanupTestDir();
}

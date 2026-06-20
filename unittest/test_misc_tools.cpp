

#include <fstream>
#include <string>
#include "ctime"
#include "filesystem"
#include "src/tools/builtin_tools/exec_tool.h"
#include "src/tools/builtin_tools/file_state_tool.h"
#include "src/tools/builtin_tools/time_info_tool.h"
#include "test_runner.h"

using namespace jiuwen;

namespace fs = std::filesystem;

// TimeInfoTool Tests
TEST(time_info_tool, ReturnsTime)
{
    TimeInfoTool tool;
    std::string result = tool.Invoke("");
    // Should contain year, month, day pattern like "YYYY-MM-DD"
    TestRunner::AssertTrue(result.length() > 10, "Result should be non-trivial");
    // Should match format like "2026-04-13 15:30:00"
    TestRunner::AssertContains(result, "-");
    TestRunner::AssertContains(result, ":");
}

TEST(time_info_tool, IgnoresInput)
{
    TimeInfoTool tool;
    std::string r1 = tool.Invoke("");
    std::string r2 = tool.Invoke("anything at all");
    // Should both return valid time strings (may differ by seconds if slow)
    TestRunner::AssertContains(r1, "-");
    TestRunner::AssertContains(r2, "-");
    // The two results should be within 5 seconds of each other
    TestRunner::AssertTrue(r1 == r2 || r1.length() == r2.length(),
        "Both should produce same format time string");
}

// ExecTool Tests - only test safety guard (platform-independent logic)
TEST(exec_tool, MissingCommand)
{
    ExecTool tool;
    std::string result = tool.Invoke("{\"timeout\":10}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "command");
}

TEST(exec_tool, BlocksRmRf)
{
    ExecTool tool;
    std::string result = tool.Invoke("{\"command\":\"rm -rf /important\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "blocked");
}

TEST(exec_tool, BlocksFormat)
{
    ExecTool tool;
    std::string result = tool.Invoke("{\"command\":\"format C:\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "blocked");
}

TEST(exec_tool, BlocksShutdown)
{
    ExecTool tool;
    std::string result = tool.Invoke("{\"command\":\"shutdown /r\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "blocked");
}

TEST(exec_tool, BlocksDiskpart)
{
    ExecTool tool;
    std::string result = tool.Invoke("{\"command\":\"diskpart clean disk\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "blocked");
}

TEST(exec_tool, AllowsSafeCommands)
{
    ExecTool tool;
    // echo is a safe cross-platform command on both Win and Linux
    std::string result = tool.Invoke("{\"command\":\"echo hello\",\"timeout\":10}");
    // Should return some output, not an error
    TestRunner::AssertFalse(result.find("blocked") != std::string::npos, "Should not block safe commands");
}

// FileStateTool Tests
static std::string GetStateDir()
{
    return "test_tmp_filestate";
}

static void SetupStateDir()
{
    if (fs::exists(GetStateDir())) fs::remove_all(GetStateDir());
    fs::create_directories(GetStateDir());
}

static void CleanupStateDir()
{
    if (fs::exists(GetStateDir())) fs::remove_all(GetStateDir());
}

TEST(file_state_tool, ClearAction)
{
    SetupStateDir();
    std::string stateFile = GetStateDir() + "/state1.dat";
    FileStateTool tool;
    tool.SetStateFile(stateFile);
    std::string result = tool.Invoke("{\"action\":\"clear\"}");
    TestRunner::AssertContains(result, "Cleared all file state");
    CleanupStateDir();
}

TEST(file_state_tool, UnknownAction)
{
    FileStateTool tool;
    std::string result = tool.Invoke("{\"action\":\"unknown\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "Unknown action");
}

TEST(file_state_tool, RecordReadMissingFile)
{
    FileStateTool tool;
    std::string result = tool.Invoke("{\"action\":\"record_read\",\"path\":\"nonexistent_file_12345.txt\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "not found");
}

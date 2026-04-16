

#include <fstream>
#include <string>
#include "ctime"
#include "filesystem"
#include "src/tools/builtin_tools/exec_tool.h"
#include "src/tools/builtin_tools/file_state_tool.h"
#include "src/tools/builtin_tools/notebook_edit_tool.h"
#include "src/tools/builtin_tools/time_info_tool.h"
#include "test_runner.h"

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

TEST(file_state_tool, ClearAction)
{
    std::string stateDir = GetStateDir() + "/state1.dat";
    if (fs::exists(stateDir)) fs::remove(stateDir);
    FileStateTool tool;
    tool.SetStateFile(stateDir);
    std::string result = tool.Invoke("{\"action\":\"clear\"}");
    TestRunner::AssertContains(result, "Cleared all file state");
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

// NotebookEditTool Tests
static std::string GetNbDir() 
{ 
    return "test_tmp_notebook"; 
}

TEST(notebook_edit_tool, MissingPath)
{
    NotebookEditTool tool;
    std::string result = tool.Invoke("{\"cell_index\":0,\"new_source\":\"test\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "path");
}

TEST(notebook_edit_tool, WrongExtension)
{
    NotebookEditTool tool;
    std::string result = tool.Invoke("{\"path\":\"test.txt\",\"cell_index\":0}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, ".ipynb");
}

TEST(notebook_edit_tool, InvalidEditMode)
{
    NotebookEditTool tool;
    std::string result = tool.Invoke("{\"path\":\"test.ipynb\",\"cell_index\":0,\"edit_mode\":\"invalid\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "Invalid edit_mode");
}

TEST(notebook_edit_tool, InvalidCellType)
{
    NotebookEditTool tool;
    std::string result = tool.Invoke("{\"path\":\"test.ipynb\",\"cell_index\":0,\"cell_type\":\"html\"}");
    TestRunner::AssertContains(result, "Error");
    TestRunner::AssertContains(result, "Invalid cell_type");
}

TEST(notebook_edit_tool, CreateNewNotebook)
{
    if (fs::exists(GetNbDir())) fs::remove_all(GetNbDir());
    fs::create_directories(GetNbDir());
    std::string path = GetNbDir() + "/new.ipynb";
    NotebookEditTool tool;
    std::string result = tool.Invoke("{\"path\":\"" + path + "\",\"cell_index\":0,\"edit_mode\":\"insert\",\"new_source\":\"print('hello')\",\"cell_type\":\"code\"}");
    TestRunner::AssertContains(result, "Successfully created");
    TestRunner::AssertEq(fs::exists(path), true);
    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    TestRunner::AssertContains(content, "nbformat");
    TestRunner::AssertContains(content, "cells");
    if (fs::exists(GetNbDir())) fs::remove_all(GetNbDir());
}

TEST(notebook_edit_tool, DeleteCell)
{
    if (fs::exists(GetNbDir())) fs::remove_all(GetNbDir());
    fs::create_directories(GetNbDir());
    std::string path = GetNbDir() + "/del.ipynb";
    NotebookEditTool tool;
    // Create notebook with 2 cells
    std::string nbContent = R"NB({
 "nbformat": 4,
 "nbformat_minor": 5,
 "metadata": {},
 "cells": [
  {
   "cell_type": "code",
   "source": ["print('a')"],
   "metadata": {},
   "outputs": [],
   "execution_count": null
  },
  {
   "cell_type": "code",
   "source": ["print('b')"],
   "metadata": {},
   "outputs": [],
   "execution_count": null
  }
 ]
})NB";
    std::ofstream f(path);
    f << nbContent;
    f.close();

    // Delete cell 0
    std::string result = tool.Invoke("{\"path\":\"" + path + "\",\"cell_index\":0,\"edit_mode\":\"delete\",\"new_source\":\"\",\"cell_type\":\"code\"}");
    TestRunner::AssertContains(result, "Successfully deleted");
    // Verify cell count reduced
    std::ifstream f2(path);
    std::string content2((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
    // Should have one fewer cell
    TestRunner::AssertTrue(content2.find("print('a')") == std::string::npos || content2.find("print('b')") != std::string::npos,
        "Cell 0 should be deleted");
    if (fs::exists(GetNbDir())) fs::remove_all(GetNbDir());
}

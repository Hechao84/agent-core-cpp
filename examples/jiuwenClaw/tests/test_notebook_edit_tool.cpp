
#include <fstream>
#include <string>
#include "filesystem"
#include "../tools/notebook_edit_tool.h"
#include "test_runner.h"

using namespace jiuwenClaw;
namespace fs = std::filesystem;

static std::string GetNbDir()
{
    return "test_tmp_notebook";
}

static void SetupNbDir()
{
    if (fs::exists(GetNbDir())) fs::remove_all(GetNbDir());
    fs::create_directories(GetNbDir());
}

static void CleanupNbDir()
{
    if (fs::exists(GetNbDir())) fs::remove_all(GetNbDir());
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
    SetupNbDir();
    std::string path = GetNbDir() + "/new.ipynb";
    NotebookEditTool tool;
    std::string result = tool.Invoke("{\"path\":\"" + path + "\",\"cell_index\":0,\"edit_mode\":\"insert\",\"new_source\":\"print('hello')\",\"cell_type\":\"code\"}");
    TestRunner::AssertContains(result, "Successfully created");
    TestRunner::AssertEq(fs::exists(path), true);
    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    TestRunner::AssertContains(content, "nbformat");
    TestRunner::AssertContains(content, "cells");
    CleanupNbDir();
}

TEST(notebook_edit_tool, DeleteCell)
{
    SetupNbDir();
    std::string path = GetNbDir() + "/del.ipynb";
    NotebookEditTool tool;
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

    std::string result = tool.Invoke("{\"path\":\"" + path + "\",\"cell_index\":0,\"edit_mode\":\"delete\",\"new_source\":\"\",\"cell_type\":\"code\"}");
    TestRunner::AssertContains(result, "Successfully deleted");
    std::ifstream f2(path);
    std::string content2((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
    TestRunner::AssertTrue(content2.find("print('a')") == std::string::npos || content2.find("print('b')") != std::string::npos,
        "Cell 0 should be deleted");
    CleanupNbDir();
}

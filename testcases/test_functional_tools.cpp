#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include "agent.h"
#include "resource_manager.h"
#include "read_file_tool.h"
#include "write_file_tool.h"
#include "edit_file_tool.h"
#include "list_dir_tool.h"
#include "glob_tool.h"
#include "grep_tool.h"
#include "exec_tool.h"
#include "time_info_tool.h"
#include "file_state_tool.h"
#include "notebook_edit_tool.h"

namespace fs = std::filesystem;

struct TestResult {
    std::string category;
    std::string test_name;
    std::string input;
    std::string expected_keyword;
    std::string actual_output;
    bool passed;
};

std::vector<TestResult> all_results;

void RunAndCheck(const std::string& category, const std::string& name,
                 const std::string& input, const std::string& expected_kw,
                 std::function<std::string()> fn) {
    try {
        std::string actual = fn();
        bool pass = actual.find(expected_kw) != std::string::npos;
        all_results.push_back({category, name, input, expected_kw, actual, pass});
        std::cout << "  [" << (pass ? "PASS" : "FAIL") << "] " << name << "\n";
        if (!pass) {
            std::cout << "    Expected to find: '" << expected_kw << "'\n";
            std::cout << "    Got: '" << actual.substr(0, 200) << "'\n";
        }
    } catch (const std::exception& e) {
        all_results.push_back({category, name, input, expected_kw, e.what(), false});
        std::cout << "  [FAIL] " << name << " -> exception: " << e.what() << "\n";
    }
}

void RunAndCheckEq(const std::string& category, const std::string& name,
                   const std::string& input, const std::string& expected_exact,
                   std::function<std::string()> fn) {
    try {
        std::string actual = fn();
        bool pass = actual == expected_exact;
        all_results.push_back({category, name, input, expected_exact, actual, pass});
        std::cout << "  [" << (pass ? "PASS" : "FAIL") << "] " << name << "\n";
        if (!pass) {
            std::cout << "    Expected: '" << expected_exact << "'\n";
            std::cout << "    Got:      '" << actual.substr(0, 200) << "'\n";
        }
    } catch (const std::exception& e) {
        all_results.push_back({category, name, input, expected_exact, e.what(), false});
        std::cout << "  [FAIL] " << name << " -> exception: " << e.what() << "\n";
    }
}

void TestTimeInfo() {
    std::cout << "\n[1] TimeInfoTool\n";
    TimeInfoTool tool;
    RunAndCheck("time_info", "Returns_time", "", "-",
        [&tool]() { return tool.Invoke(""); });
}

void TestReadWriteFiles() {
    std::cout << "\n[2] Read/Write File Tools\n";
    std::string td = "test_tmp_func";
    if (fs::exists(td)) fs::remove_all(td);
    fs::create_directories(td);

    WriteFileTool wtool;
    ReadFileTool rtool;

    RunAndCheck("write_file", "Write_success", "{\"path\":\"test_tmp_func/f.txt\",\"content\":\"hello\"}",
        "Successfully wrote",
        [&wtool]() { return wtool.Invoke("{\"path\":\"test_tmp_func/f.txt\",\"content\":\"hello\"}"); });

    RunAndCheck("read_file", "Read_success", "{\"path\":\"test_tmp_func/f.txt\"}",
        "1| hello",
        [&rtool]() { return rtool.Invoke("{\"path\":\"test_tmp_func/f.txt\"}"); });

    RunAndCheck("read_file", "Missing_path", "{}",
        "Error",
        [&rtool]() { return rtool.Invoke("{}"); });

    RunAndCheck("read_file", "File_not_found", "{\"path\":\"nonexistent.txt\"}",
        "Error",
        [&rtool]() { return rtool.Invoke("{\"path\":\"nonexistent.txt\"}"); });

    // Write multi-line and test pagination
    std::ofstream mf(td + "/multi.txt");
    for (int i = 1; i <= 20; ++i) mf << "line" << i << "\n";
    mf.close();

    RunAndCheck("read_file", "With_offset_limit",
        "{\"path\":\"test_tmp_func/multi.txt\",\"offset\":5,\"limit\":3}",
        "5| line5",
        [&rtool]() { return rtool.Invoke("{\"path\":\"test_tmp_func/multi.txt\",\"offset\":5,\"limit\":3}"); });

    fs::remove_all(td);
}

void TestEditFile() {
    std::cout << "\n[3] EditFileTool\n";
    std::string td = "test_tmp_edit";
    if (fs::exists(td)) fs::remove_all(td);
    fs::create_directories(td);

    EditFileTool tool;

    // Create file first
    std::ofstream f(td + "/edit.txt");
    f << "original text\n";
    f.close();

    RunAndCheck("edit_file", "Simple_replace",
        "{\"path\":\"test_tmp_edit/edit.txt\",\"old_text\":\"original text\",\"new_text\":\"new text\"}",
        "Successfully edited",
        [&tool]() { return tool.Invoke("{\"path\":\"test_tmp_edit/edit.txt\",\"old_text\":\"original text\",\"new_text\":\"new text\"}"); });

    RunAndCheck("edit_file", "Old_text_not_found",
        "{\"path\":\"test_tmp_edit/edit.txt\",\"old_text\":\"NOTHERE\",\"new_text\":\"x\"}",
        "Error",
        [&tool]() { return tool.Invoke("{\"path\":\"test_tmp_edit/edit.txt\",\"old_text\":\"NOTHERE\",\"new_text\":\"x\"}"); });

    fs::remove_all(td);
}

void TestListDir() {
    std::cout << "\n[4] ListDirTool\n";
    std::string td = "test_tmp_ls";
    if (fs::exists(td)) fs::remove_all(td);
    fs::create_directories(td);
    fs::create_directories(td + "/sub");
    std::ofstream(td + "/a.cpp") << "a";
    std::ofstream(td + "/b.py") << "b";

    ListDirTool tool;

    RunAndCheck("list_dir", "List_directory",
        "{\"path\":\"test_tmp_ls\"}",
        "a.cpp",
        [&tool]() { return tool.Invoke("{\"path\":\"test_tmp_ls\"}"); });

    RunAndCheck("list_dir", "Recursive",
        "{\"path\":\"test_tmp_ls\",\"recursive\":true}",
        "sub/",
        [&tool]() { return tool.Invoke("{\"path\":\"test_tmp_ls\",\"recursive\":true}"); });

    fs::remove_all(td);
}

void TestGlob() {
    std::cout << "\n[5] GlobTool\n";
    std::string td = "test_tmp_glob";
    if (fs::exists(td)) fs::remove_all(td);
    fs::create_directories(td);
    std::ofstream(td + "/test.cpp") << "t";
    std::ofstream(td + "/main.cpp") << "m";
    std::ofstream(td + "/readme.md") << "r";

    GlobTool tool;

    RunAndCheck("glob", "Match_cpp",
        "{\"path\":\"test_tmp_glob\",\"pattern\":\"*.cpp\"}",
        ".cpp",
        [&tool]() { return tool.Invoke("{\"path\":\"test_tmp_glob\",\"pattern\":\"*.cpp\"}"); });

    RunAndCheck("glob", "No_match",
        "{\"path\":\"test_tmp_glob\",\"pattern\":\"*.xyz\"}",
        "No paths matched",
        [&tool]() { return tool.Invoke("{\"path\":\"test_tmp_glob\",\"pattern\":\"*.xyz\"}"); });

    fs::remove_all(td);
}

void TestGrep() {
    std::cout << "\n[6] GrepTool\n";
    std::string td = "test_tmp_grep";
    if (fs::exists(td)) fs::remove_all(td);
    fs::create_directories(td);
    std::ofstream(td + "/hello.cpp") << "std::cout << \"hello\";\n";
    std::ofstream(td + "/world.cpp") << "std::cout << \"world\";\n";

    GrepTool tool;

    RunAndCheck("grep", "Find_in_files",
        "{\"path\":\"test_tmp_grep\",\"pattern\":\"hello\",\"output_mode\":\"files_with_matches\"}",
        "hello.cpp",
        [&tool]() { return tool.Invoke("{\"path\":\"test_tmp_grep\",\"pattern\":\"hello\",\"output_mode\":\"files_with_matches\"}"); });

    RunAndCheck("grep", "No_match",
        "{\"path\":\"test_tmp_grep\",\"pattern\":\"NOTFOUND123\"}",
        "No matches found",
        [&tool]() { return tool.Invoke("{\"path\":\"test_tmp_grep\",\"pattern\":\"NOTFOUND123\"}"); });

    fs::remove_all(td);
}

void TestExec() {
    std::cout << "\n[7] ExecTool\n";
    ExecTool tool;

    RunAndCheck("exec", "Safety_guard_rm_rf",
        "{\"command\":\"rm -rf /important\"}",
        "blocked",
        [&tool]() { return tool.Invoke("{\"command\":\"rm -rf /important\"}"); });

    RunAndCheck("exec", "Safe_command",
        "{\"command\":\"echo test_exec\",\"timeout\":10}",
        "Exit code",
        [&tool]() { return tool.Invoke("{\"command\":\"echo test_exec\",\"timeout\":10}"); });
}

void TestFileState() {
    std::cout << "\n[8] FileStateTool\n";
    FileStateTool tool;

    RunAndCheck("file_state", "Clear",
        "{\"action\":\"clear\"}",
        "Cleared all file state",
        [&tool]() { return tool.Invoke("{\"action\":\"clear\"}"); });

    RunAndCheck("file_state", "Unknown_action",
        "{\"action\":\"unknown_action\"}",
        "Error",
        [&tool]() { return tool.Invoke("{\"action\":\"unknown_action\"}"); });
}

void TestNotebookEdit() {
    std::cout << "\n[9] NotebookEditTool\n";
    std::string td = "test_tmp_nb";
    if (fs::exists(td)) fs::remove_all(td);
    fs::create_directories(td);

    NotebookEditTool tool;

    RunAndCheck("notebook", "Wrong_extension",
        "{\"path\":\"test.txt\",\"cell_index\":0}",
        "Error",
        [&tool]() { return tool.Invoke("{\"path\":\"test.txt\",\"cell_index\":0}"); });

    RunAndCheck("notebook", "Invalid_edit_mode",
        "{\"path\":\"x.ipynb\",\"cell_index\":0,\"edit_mode\":\"bad\"}",
        "Error",
        [&tool]() { return tool.Invoke("{\"path\":\"x.ipynb\",\"cell_index\":0,\"edit_mode\":\"bad\"}"); });

    fs::remove_all(td);
}

void TestResourceManager() {
    std::cout << "\n[10] ResourceManager\n";
    auto& rm = ResourceManager::GetInstance();

    auto tools = rm.GetAvailableTools();
    std::cout << "  Available tools: " << tools.size() << "\n";
    for (const auto& t : tools) {
        bool ok = rm.HasTool(t);
        all_results.push_back({"resource_manager", "HasTool_" + t, t, "true", ok ? "true" : "false", ok});
        std::cout << "  [" << (ok ? "PASS" : "FAIL") << "] HasTool: " << t << "\n";
    }
}

void GenerateReport() {
    int total = static_cast<int>(all_results.size());
    int passed = 0;
    for (const auto& r : all_results) if (r.passed) passed++;

    std::ofstream report("TEST_REPORT.md");
    if (!report.is_open()) return;

    report << "# Test Report\n\n";
    report << "**Total**: " << total << " | **Passed**: " << passed
           << " | **Failed**: " << (total - passed)
           << " | **Pass Rate**: " << (total > 0 ? (100 * passed / total) : 0) << "%\n\n";

    report << "## Results Table\n\n";
    report << "| # | Category | Test Name | Input | Expected Keyword | Actual Output | Status |\n";
    report << "|---|----------|-----------|-------|------------------|---------------|--------|\n";

    int idx = 1;
    for (const auto& r : all_results) {
        std::string status = r.passed ? "PASS" : "FAIL";
        report << "| " << idx++
               << " | " << r.category
               << " | " << r.test_name
               << " | `" << r.input.substr(0, 60) << "`"
               << " | `" << r.expected_keyword.substr(0, 40) << "`"
               << " | `" << r.actual_output.substr(0, 100) << "`"
               << " | " << status << " |\n";
    }

    report.close();
    std::cout << "\nReport generated: TEST_REPORT.md\n";
    std::cout << "Total: " << total << " | Passed: " << passed << " | Failed: " << (total - passed) << "\n";
}

int main() {
    std::cout << "========================================\n";
    std::cout << "     Jiuwen-Lite Functional Tests\n";
    std::cout << "========================================\n";

    // Register all builtin tools
    auto& rm = ResourceManager::GetInstance();

    TestTimeInfo();
    TestReadWriteFiles();
    TestEditFile();
    TestListDir();
    TestGlob();
    TestGrep();
    TestExec();
    TestFileState();
    TestNotebookEdit();
    TestResourceManager();

    std::cout << "\n========================================\n";
    GenerateReport();

    return 0;
}

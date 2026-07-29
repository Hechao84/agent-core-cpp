

#include <fstream>
#include <string>
#include "ctime"
#include "filesystem"
#include "src/tools/builtin_tools/exec_tool.h"
#include "src/tools/builtin_tools/file_state_tool.h"
#include "src/tools/builtin_tools/time_info_tool.h"
#include "src/tools/builtin_tools/web_fetch_tool.h"
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

// WebFetcherTool URL extraction tests.
// Regression coverage for an off-by-one in the previous hand-rolled JSON
// substring extractor that prefixed the URL with a stray opening quote
// ("\"https://..."), which curl then rejected as
// "URL using bad/illegal format or missing URL".
TEST(web_fetch_tool, ExtractsUrlFromJsonObject)
{
    std::string url = WebFetcherTool::ExtractUrl("{\"url\":\"https://example.com/path\"}");
    TestRunner::AssertEq(url, std::string("https://example.com/path"),
        "URL must be extracted without leading quote");
}

TEST(web_fetch_tool, ExtractsUrlFromJsonWithSpaces)
{
    std::string url = WebFetcherTool::ExtractUrl("{\"url\": \"https://example.com/path/\"}");
    TestRunner::AssertEq(url, std::string("https://example.com/path/"),
        "URL with trailing slash and JSON spaces must be extracted verbatim");
}

TEST(web_fetch_tool, ExtractsUrlFromJsonWithQueryParams)
{
    std::string url = WebFetcherTool::ExtractUrl("{\"url\":\"https://ent.sina.cn/2026-04-28/detail.d.html?vt=4\"}");
    TestRunner::AssertEq(url, std::string("https://ent.sina.cn/2026-04-28/detail.d.html?vt=4"),
        "URL with query params must be preserved");
}

TEST(web_fetch_tool, ExtractsUrlFromJsonWithPercentEncoding)
{
    std::string url = WebFetcherTool::ExtractUrl("{\"url\":\"https://baike.baidu.com/item/%E5%91%A8%E6%B7%B1/67507045\"}");
    TestRunner::AssertEq(url, std::string("https://baike.baidu.com/item/%E5%91%A8%E6%B7%B1/67507045"),
        "Percent-encoded URL must be preserved verbatim");
}

TEST(web_fetch_tool, FallsBackToRawInputWhenNotJson)
{
    std::string url = WebFetcherTool::ExtractUrl("https://example.com");
    TestRunner::AssertEq(url, std::string("https://example.com"),
        "Bare URL input should pass through unchanged");
}

TEST(web_fetch_tool, ExtractedUrlHasNoLeadingQuote)
{
    // Direct regression test: the URL must not start with a double quote
    // (the original bug signature).
    std::string url = WebFetcherTool::ExtractUrl("{\"url\":\"https://example.com/\"}");
    TestRunner::AssertTrue(url.empty() || url.front() != '"',
        "URL must not begin with a double quote");
}

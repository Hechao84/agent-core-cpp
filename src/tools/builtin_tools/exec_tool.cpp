#include "src/tools/builtin_tools/exec_tool.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
    #include <windows.h>
#else
    // Linux/Unix specific alternatives if needed, or simply exclude logic
#endif

namespace jiuwen {

static std::string ParseStringField(const std::string& json, const std::string& key)
{
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";
    size_t colonPos = json.find(':', keyPos + searchKey.length());
    if (colonPos == std::string::npos) return "";
    size_t valStart = json.find_first_not_of(" \t", colonPos + 1);
    if (valStart == std::string::npos) return "";
    if (json[valStart] != '"') return "";
    size_t valEnd = valStart + 1;
    while (valEnd < json.length()) {
        if (json[valEnd] == '\\' && valEnd + 1 < json.length()) { 
            valEnd += 2; 
            continue; 
        }
        if (json[valEnd] == '"') {
            break;
        }
        valEnd++;
    }
    return json.substr(valStart + 1, valEnd - valStart - 1);
}

static int ParseIntField(const std::string& json, const std::string& key, int defaultVal)
{
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return defaultVal;
    size_t colonPos = json.find(':', keyPos + searchKey.length());
    if (colonPos == std::string::npos) return defaultVal;
    size_t valStart = json.find_first_not_of(" \t", colonPos + 1);
    if (valStart == std::string::npos) return defaultVal;
    try { 
        return std::stoi(json.substr(valStart)); 
    } catch (...) { 
        return defaultVal; 
    }
}

ExecTool::ExecTool()
    : Tool("exec", "Execute a shell command and return its output. "
         "Use -y or --yes flags to avoid interactive prompts. "
         "Output is truncated at 10000 chars; timeout defaults to 60s. "
         "Input: JSON with 'command' (required), "
         "'working_dir' (optional), 'timeout' (default 60, max 600).",
         {{"command", "The shell command to execute", "string", true},
          {"working_dir", "Optional working directory", "string", false},
          {"timeout", "Timeout in seconds (default 60, max 600)", "integer", false}}){} std::string ExecTool::Invoke(const std::string& input)
{
    // Extract command from JSON
    std::string command = ParseStringField(input, "command");
    if (command.empty()) {
        return "Error: 'command' parameter is required";
    }

    std::string workingDir = ParseStringField(input, "working_dir");
    int timeout = ParseIntField(input, "timeout", 60);
    if (timeout > 600) timeout = 600;
    if (timeout < 1) timeout = 60;
    timeout_ = timeout;

    // Safety guard: block dangerous commands
    std::string guardError = GuardCommand(command);
    if (!guardError.empty()) return guardError;

#ifdef _WIN32
    return ExecuteWindows(command, workingDir);
#else
    return ExecutePosix(command, workingDir, timeout_);
#endif
}

std::string ExecTool::GuardCommand(const std::string& command)
{
    std::string lowerCmd = command;
    std::transform(lowerCmd.begin(), lowerCmd.end(), lowerCmd.begin(), ::tolower);

    static const std::vector<std::string> denyPatterns = {
        R"(rm\s+-[rf]{1,2})",
        R"(del\s+/[fq])",
        R"(rmdir\s+/s)",
        R"(format\b)",
        R"(\b(mkfs|diskpart)\b)",
        R"(\bdd\s+if=)",
        R"(>\s*/dev/sd)",
        R"(\b(shutdown|reboot|poweroff)\b)",
    };

    for (const auto& pat : denyPatterns) {
        try {
            std::regex re(pat, std::regex::icase);
            if (std::regex_search(lowerCmd, re)) {
                return "Error: Command blocked by safety guard (dangerous pattern detected)";
            }
        } catch (...){

        } 
    }
    return "";
}

#ifdef _WIN32
std::string ExecTool::ExecuteWindows(const std::string& command, const std::string& workingDir)
{
    // Build command with cd prefix if working_dir is specified
    std::string fullCmd = command;
    std::string tempDir;
    if (!workingDir.empty()) {
        tempDir = workingDir;
        fullCmd = "cd /d \"" + workingDir + "\" && " + command;
    }

    // Create temp file for output
    char tempFile[MAX_PATH];
    GetTempPathA(MAX_PATH, tempFile);
    char uniqueName[MAX_PATH];
    GetTempFileNameA(tempFile, "exec", 0, uniqueName);
    std::string outFile(uniqueName);
    std::string errFile = outFile + ".err";

    // Build cmd.exe command with redirection
    std::string cmdLine = "cmd.exe /c \"" + fullCmd + "\" > \"" + outFile + "\" 2> \"" + errFile + "\"";

    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    bool success = CreateProcessA(
        NULL, &cmdLine[0], NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi
    );

    if (!success) {
        DeleteFileA(outFile.c_str());
        DeleteFileA(errFile.c_str());
        return "Error: Failed to execute command";
    }

    // Wait with timeout
    DWORD waitResult = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_) * 1000);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        DeleteFileA(outFile.c_str());
        DeleteFileA(errFile.c_str());
        return "Error: Command timed out after " + std::to_string(timeout_) + " seconds";
    }

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Read output
    std::string result = ReadFileContent(outFile);
    std::string stderrContent = ReadFileContent(errFile);

    DeleteFileA(outFile.c_str());
    DeleteFileA(errFile.c_str());

    std::ostringstream oss;
    if (!result.empty()) oss << result;
    if (!stderrContent.empty()) oss << "STDERR:\n" << stderrContent << "\n";
    oss << "\nExit code: " << exitCode;

    return TruncateOutput(oss.str());
}

std::string ExecTool::ReadFileContent(const std::string& filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return "";
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return content;
}
#else
std::string ExecTool::ExecutePosix(const std::string& command, const std::string& workingDir, int timeoutSec)
{
    std::string fullCmd = command;
    if (!workingDir.empty()) {
        fullCmd = "cd " + workingDir + " && " + command;
    }

    // Add timeout wrapper
    fullCmd = "timeout " + std::to_string(timeoutSec) + "s " + fullCmd + " 2>&1";

    std::string result;
    FILE* pipe = popen(fullCmd.c_str(), "r");
    if (!pipe) {
        return "Error: Failed to execute command";
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }

    int exitCode = pclose(pipe);
    if (exitCode == 124) {
        return "Error: Command timed out after " + std::to_string(timeoutSec) + " seconds";
    }

    if (result.empty()) result = "(no output)";
    result += "\nExit code: " + std::to_string(exitCode);

    return TruncateOutput(result);
}
#endif

std::string ExecTool::TruncateOutput(const std::string& output)
{
    static const int kMaxOutput = 10000;
    if (static_cast<int>(output.length()) <= kMaxOutput) return output;

    int half = kMaxOutput / 2;
    std::string truncated = output.substr(0, half);
    truncated += "\n\n... (" + std::to_string(output.length() - kMaxOutput) + " chars truncated) ...\n\n";
    truncated += output.substr(output.length() - half);
    return truncated;
}

} // namespace jiuwen

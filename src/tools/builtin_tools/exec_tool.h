#pragma once

#include <string>
#include "include/tool.h"

class ExecTool : public Tool {
public:
    ExecTool();
    std::string Invoke(const std::string& input) override;
private:
    int timeout_ = 60;
#ifdef _WIN32
    std::string ExecuteWindows(const std::string& command, const std::string& workingDir);
    std::string ReadFileContent(const std::string& filePath);
#else
    std::string ExecutePosix(const std::string& command, const std::string& workingDir, int timeoutSec);
#endif
    std::string GuardCommand(const std::string& command);
    std::string TruncateOutput(const std::string& output);
};

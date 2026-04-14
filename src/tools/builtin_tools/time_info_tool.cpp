#include "time_info_tool.h"
#include <ctime>
#include <string>

TimeInfoTool::TimeInfoTool() : Tool("time_info", "Get the current date and time", {}) {}

std::string TimeInfoTool::Invoke(const std::string& /*input*/) {
    std::time_t now = std::time(nullptr);
    char buf[100];
    std::tm *t = std::localtime(&now);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S (Local Time)", t);
    return std::string(buf);
}

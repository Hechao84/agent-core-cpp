#include "src/utils/logger.h"
#include <ctime>
#include <cstring>
#include <fstream>
#include <mutex>

namespace {
    LogLevel g_globalLevel = INFO;
    std::mutex g_logMutex;
}

void Logger::SetGlobalLevel(LogLevel level)
{
    g_globalLevel = level;
}

LogLevel Logger::GetGlobalLevel()
{
    return g_globalLevel;
}

bool Logger::ShouldLog(LogLevel level)
{
    return level >= g_globalLevel;
}

static const char* GetLevelName(LogLevel level)
{
    switch (level) {
        case DEBUG: return "DEBUG";
        case INFO: return "INFO";
        case WARNING: return "WARN";
        case ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

LogStream::LogStream(const char* file, int line, LogLevel level)
    : file_(file), line_(line), level_(level)
{
    const char* fileName = std::strrchr(file_, '/');
    if (fileName) {
        file_ = fileName + 1;
    }
}

LogStream::~LogStream()
{
    oss_ << "\n";
    std::string msg = oss_.str();

    std::lock_guard<std::mutex> lock(g_logMutex);

    std::time_t t = std::time(nullptr);
    std::tm tmBuf;
    std::tm* tmPtr = nullptr;

#ifdef _WIN32
    tmPtr = std::localtime(&t);
    if (tmPtr) tmBuf = *tmPtr;
#else
    tmPtr = localtime_r(&t, &tmBuf);
#endif

    if (tmPtr) {
        char timeStr[20];
        std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", tmPtr);
        
        std::ofstream logFile("jiuwen-lite.log", std::ios::app);
        if (logFile.is_open()) {
            logFile << "[" << timeStr << "] [" << GetLevelName(level_) << "] ["
                    << file_ << ":" << line_ << "] " << msg;
            logFile.flush();
        }
    }
}

#include "logger.h"

#include <ctime>
#include <cstring>
#include <fstream>
#include <mutex>

namespace {
    jiuwenClaw::LogLevel g_globalLevel = jiuwenClaw::INFO;
    std::mutex g_logMutex;

    const char* GetLevelName(jiuwenClaw::LogLevel level)
    {
        switch (level) {
            case jiuwenClaw::DBG: return "DEBUG";
            case jiuwenClaw::INFO: return "INFO";
            case jiuwenClaw::WARN: return "WARN";
            case jiuwenClaw::ERR: return "ERROR";
            default: return "UNKNOWN";
        }
    }
} // namespace

namespace jiuwenClaw {

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

} // namespace jiuwenClaw

#ifndef INCLUDE_LOGGER_H_
#define INCLUDE_LOGGER_H_

#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

enum LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3
};

class LogStream {
    public:
        LogStream(const char* file, int line, LogLevel level);
        ~LogStream();

        template <typename T>
        LogStream& operator<<(const T& value)
        {
            oss_ << value;
            return *this;
        }

        using Manipulator = LogStream& (*)(LogStream&);
        LogStream& operator<<(Manipulator manip) { return manip(*this); }

    private:
        std::ostringstream oss_;
        const char* file_;
        int line_;
        LogLevel level_;
};

class Logger {
    public:
        static void SetGlobalLevel(LogLevel level);
        static LogLevel GetGlobalLevel();
        static bool ShouldLog(LogLevel level);

    private:
        static LogLevel globalLevel_;
        static std::mutex logMutex_;
};

#define LOG(level) \
    if (Logger::ShouldLog(level)) \
    LogStream(__FILE__, __LINE__, level)

#endif  // INCLUDE_LOGGER_H_

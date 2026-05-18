#include "src/utils/data_dir.h"
#include <filesystem>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

namespace jiuwen {

// Make safe filename from session ID
static std::string MakeSafeFilename(const std::string& name)
{
    std::string safe = name;
    for (char& c : safe) {
        if (c == ':' || c == '/' || c == '\\' || c == ' ' || c == '?' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    return safe;
}

DataDir::DataDir() : basePath_("./data")
{
    EnsureDirectory(basePath_);
}

DataDir::DataDir(const std::string& basePath) : basePath_(basePath)
{
    EnsureDirectory(basePath_);
}

void DataDir::SetBasePath(const std::string& path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    basePath_ = path;
    EnsureDirectory(basePath_);
}

std::string DataDir::GetBasePath() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return basePath_;
}

std::string DataDir::GetPath(const std::string& dataType) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string subPath = basePath_ + "/" + dataType;
    EnsureDirectory(subPath);
    return subPath;
}

std::string DataDir::GetFilePath(const std::string& dataType, const std::string& fileName) const
{
    std::string dir = GetPath(dataType);
    return dir + "/" + fileName;
}

std::string DataDir::GetSessionPath(const std::string& sessionId, const std::string& subPath) const
{
    std::string safeName = MakeSafeFilename(sessionId);
    std::string base;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        base = basePath_;
    }
    std::string path = base + "/sessions/" + safeName;
    if (!subPath.empty()) {
        path += "/" + subPath;
    }
    EnsureDirectory(path);
    return path;
}

std::string DataDir::GetSessionFilePath(const std::string& sessionId, const std::string& fileName) const
{
    return GetSessionPath(sessionId) + "/" + fileName;
}

std::string DataDir::GetMemoryPath() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return basePath_ + "/memory/MEMORY.md";
}

std::string DataDir::GetHeartbeatPath() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return basePath_ + "/HEARTBEAT.md";
}

std::string DataDir::GetCronJobsPath() const
{
    std::string dir = GetPath("cron");
    return dir + "/jobs.json";
}

void DataDir::EnsureDirectory(const std::string& path) const
{
    if (!fs::exists(path)) {
        fs::create_directories(path);
    }
}

// Global singleton instance (thread-safe via Meyer's singleton)
static DataDir& GetGlobalDataDir()
{
    static DataDir instance;
    return instance;
}

DataDir& GetDataDir()
{
    return GetGlobalDataDir();
}

// Allow initialization from main or agent constructor
void InitDataDir(const std::string& basePath)
{
    DataDir& dir = GetGlobalDataDir();
    dir.SetBasePath(basePath);
}

} // namespace jiuwen

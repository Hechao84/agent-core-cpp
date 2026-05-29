#include "examples/jiuwenClaw/utils/data_dir.h"

#include <filesystem>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

namespace jiuwenClaw {

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

void DataDir::EnsureDirectory(const std::string& path) const
{
    if (!fs::exists(path)) {
        fs::create_directories(path);
    }
}

static DataDir& GetGlobalDataDir()
{
    static DataDir instance;
    return instance;
}

DataDir& GetDataDir()
{
    return GetGlobalDataDir();
}

void InitDataDir(const std::string& basePath)
{
    GetGlobalDataDir().SetBasePath(basePath);
}

} // namespace jiuwenClaw

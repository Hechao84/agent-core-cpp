#include "src/utils/data_dir.h"
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

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
    basePath_ = path;
    EnsureDirectory(basePath_);
}

std::string DataDir::GetBasePath() const
{
    return basePath_;
}

std::string DataDir::GetPath(const std::string& dataType) const
{
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

// Global singleton instance
static DataDir* g_dataDir = nullptr;

DataDir& GetDataDir()
{
    if (!g_dataDir) {
        g_dataDir = new DataDir();
    }
    return *g_dataDir;
}

// Allow initialization from main or agent constructor
void InitDataDir(const std::string& basePath)
{
    delete g_dataDir;
    g_dataDir = new DataDir(basePath);
}

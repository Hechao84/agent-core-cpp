#pragma once

#include <mutex>
#include <string>

namespace jiuwenClaw {

class DataDir {
public:
    DataDir();
    explicit DataDir(const std::string& basePath);

    void SetBasePath(const std::string& path);
    std::string GetBasePath() const;

    std::string GetPath(const std::string& dataType) const;
    std::string GetFilePath(const std::string& dataType, const std::string& fileName) const;

private:
    mutable std::mutex mutex_;
    std::string basePath_;

    void EnsureDirectory(const std::string& path) const;
};

DataDir& GetDataDir();
void InitDataDir(const std::string& basePath);

} // namespace jiuwenClaw

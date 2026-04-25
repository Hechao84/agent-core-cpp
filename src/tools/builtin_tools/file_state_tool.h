#pragma once

#include <ctime>
#include <filesystem>
#include <string>
#include <map>
#include "include/tool.h"

namespace jiuwen {

struct FileState {
    long long lastMtime = 0;
    time_t lastReadTime = 0;
    time_t lastWriteTime = 0;
    int lastReadOffset = 0;
    int lastReadLimit = 2000;
};

class FileStateTool : public Tool {
public:
    FileStateTool();

    void SetStateFile(const std::string& path);
    std::string Invoke(const std::string& input) override;

private:
    std::string stateFile_;
    std::map<std::string, FileState> stateMap_;

    void LoadState();
    void SaveState();
};

} // namespace jiuwen

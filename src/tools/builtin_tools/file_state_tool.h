#pragma once
#include "tool.h"
#include <string>
#include <map>

struct FileState {
    long long lastMtime = 0;
    long long lastReadTime = 0;
    long long lastWriteTime = 0;
    int lastReadOffset = 1;
    int lastReadLimit = 2000;
};

class FileStateTool : public Tool {
public:
    FileStateTool();
    std::string Invoke(const std::string& input) override;
    void SetStateFile(const std::string& path);
private:
    std::map<std::string, FileState> stateMap_;
    std::string stateFile_ = ".jiuwen_file_state.dat";
    void LoadState();
    void SaveState();
};

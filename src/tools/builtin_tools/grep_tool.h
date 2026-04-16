#pragma once

#include <map>
#include <string>
#include <vector>
#include "filesystem"
#include "include/tool.h"
#include "regex"

namespace fs = std::filesystem;

class GrepTool : public Tool {
public:
    GrepTool();
    std::string Invoke(const std::string& input) override;
private:
    void ProcessFile(const fs::path& filePath, const fs::path& root,
                    const std::regex& re, const std::string& pattern,
                    const std::string& outputMode, int headLimit,
                    int contextBefore, int contextAfter,
                    std::vector<std::string>& matchingFiles,
                    std::vector<std::string>& blocks,
                    std::map<std::string, int>& counts,
                    std::map<std::string, double>& fileMtimes,
                    int& resultChars, bool& truncated,
                    int& skippedBinary, int& skippedLarge);
};

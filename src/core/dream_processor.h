#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "include/types.h"
#include "src/utils/tool_parser.h"

namespace jiuwen {

class Model;
class HistoryStore;

struct DreamFinding
{
    std::string type;
    std::string content;
    std::string skillName;
    std::string skillDesc;
};

class DreamProcessor
{
public:
    DreamProcessor(DreamConfig config);
    ~DreamProcessor() = default;

    bool Run(Model* model, HistoryStore* historyStore);

private:
    DreamConfig config_;

    std::string LoadFile(const std::string& path) const;
    std::string FindingsToText(const std::vector<DreamFinding>& findings) const;
    void RefreshFileCache(const std::string& workspace, const std::string& toolName, const std::string& args, std::string& memoryContent, std::string& soulContent, std::string& userContent) const;

    std::vector<DreamFinding> Phase1Analysis(Model* model, const std::string& historyText, const std::string& memoryContent, const std::string& soulContent, const std::string& userContent) const;
    bool Phase2Execution(Model* model, const std::vector<DreamFinding>& findings) const;

    std::string TruncateText(const std::string& text, int maxChars) const;
    std::string LoadDreamPhase1Prompt() const;
    std::string LoadDreamPhase2Prompt() const;
    std::string BuildPhase2Prompt(const std::vector<DreamFinding>& findings, const std::string& scratchpad, const std::string& memoryContent, const std::string& soulContent, const std::string& userContent) const;

    std::string ExecuteTool(const std::string& toolName, const std::string& input) const;
};

} // namespace jiuwen

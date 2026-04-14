#pragma once
#include "tool.h"
#include <string>
#include <vector>

class NotebookEditTool : public Tool {
public:
    NotebookEditTool();
    std::string Invoke(const std::string& input) override;
private:
    std::string EscapeJson(const std::string& input);
    std::string CreateCell(const std::string& cellType, const std::string& source);
    std::string CreateEmptyNotebook(const std::string& cellType, const std::string& source);
    void ExtractCells(const std::string& content, std::vector<std::string>& cells);
    void WriteNotebook(const std::string& filePath, const std::vector<std::string>& cells, const std::string& originalContent);
};

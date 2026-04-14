#include "notebook_edit_tool.h"
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

static std::string ParseStringField(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";
    size_t colonPos = json.find(':', keyPos + searchKey.length());
    if (colonPos == std::string::npos) return "";
    size_t valStart = json.find_first_not_of(" \t", colonPos + 1);
    if (valStart == std::string::npos) return "";
    if (json[valStart] != '"') return "";
    size_t valEnd = valStart + 1;
    while (valEnd < json.length()) {
        if (json[valEnd] == '\\' && valEnd + 1 < json.length()) { valEnd += 2; continue; }
        if (json[valEnd] == '"') break;
        valEnd++;
    }
    return json.substr(valStart + 1, valEnd - valStart - 1);
}

static int ParseIntField(const std::string& json, const std::string& key, int defaultVal) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return defaultVal;
    size_t colonPos = json.find(':', keyPos + searchKey.length());
    if (colonPos == std::string::npos) return defaultVal;
    size_t valStart = json.find_first_not_of(" \t", colonPos + 1);
    if (valStart == std::string::npos) return defaultVal;
    try { return std::stoi(json.substr(valStart)); } catch (...) { return defaultVal; }
}

NotebookEditTool::NotebookEditTool()
    : Tool("notebook_edit", "Edit a Jupyter notebook (.ipynb) cell. "
         "Modes: replace (default), insert (after target index), delete. "
         "Input: JSON with 'path' (required), 'cell_index' (0-based, required), "
         "'new_source' (for replace/insert), 'cell_type' (code|markdown, default code), "
         "'edit_mode' (replace|insert|delete, default replace).",
         {{"path", "Path to the .ipynb notebook file", "string", true},
          {"cell_index", "0-based index of the cell to edit", "integer", true},
          {"new_source", "New source content for the cell", "string", false},
          {"cell_type", "Cell type: code or markdown", "string", false},
          {"edit_mode", "Mode: replace, insert, or delete", "string", false}}) {}

std::string NotebookEditTool::Invoke(const std::string& input) {
    std::string filePath = ParseStringField(input, "path");
    if (filePath.empty()) {
        return "Error: 'path' parameter is required";
    }
    if (filePath.size() < 6 || filePath.substr(filePath.size() - 6) != ".ipynb") {
        return "Error: notebook_edit only works on .ipynb files. Use edit_file for other files.";
    }

    int cellIndex = ParseIntField(input, "cell_index", 0);
    std::string newSource = ParseStringField(input, "new_source");
    std::string cellType = ParseStringField(input, "cell_type");
    if (cellType.empty()) cellType = "code";
    std::string editMode = ParseStringField(input, "edit_mode");
    if (editMode.empty()) editMode = "replace";

    if (cellType != "code" && cellType != "markdown") {
        return "Error: Invalid cell_type '" + cellType + "'. Use 'code' or 'markdown'.";
    }
    if (editMode != "replace" && editMode != "insert" && editMode != "delete") {
        return "Error: Invalid edit_mode '" + editMode + "'. Use 'replace', 'insert', or 'delete'.";
    }

    fs::path fp(filePath);

    // Create new notebook if file doesn't exist and mode is insert
    if (!fs::exists(fp)) {
        if (editMode != "insert") {
            return "Error: File not found: " + filePath;
        }
        std::string newNotebook = CreateEmptyNotebook(cellType, newSource);
        fs::create_directories(fp.parent_path());
        std::ofstream file(filePath, std::ios::out | std::ios::trunc);
        if (!file.is_open()) return "Error: Cannot create file: " + filePath;
        file << newNotebook;
        file.close();
        return "Successfully created " + filePath + " with 1 cell";
    }

    // Read notebook
    std::ifstream inFile(filePath);
    if (!inFile.is_open()) return "Error: Cannot open file: " + filePath;
    std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
    inFile.close();

    // Simple JSON parsing for cells array
    std::vector<std::string> cells;
    ExtractCells(content, cells);

    int cellCount = static_cast<int>(cells.size());

    if (editMode == "delete") {
        if (cellIndex < 0 || cellIndex >= cellCount) {
            return "Error: cell_index " + std::to_string(cellIndex) +
                   " out of range (notebook has " + std::to_string(cellCount) + " cells)";
        }
        cells.erase(cells.begin() + cellIndex);
        WriteNotebook(filePath, cells, content);
        return "Successfully deleted cell " + std::to_string(cellIndex) + " from " + filePath;
    }

    if (editMode == "insert") {
        int insertAt = std::min(cellIndex + 1, cellCount);
        std::string newCell = CreateCell(cellType, newSource);
        cells.insert(cells.begin() + insertAt, newCell);
        WriteNotebook(filePath, cells, content);
        return "Successfully inserted cell at index " + std::to_string(insertAt) + " in " + filePath;
    }

    // Replace mode
    if (cellIndex < 0 || cellIndex >= cellCount) {
        return "Error: cell_index " + std::to_string(cellIndex) +
               " out of range (notebook has " + std::to_string(cellCount) + " cells)";
    }

    // Update the cell source and type
    std::string cell = cells[cellIndex];
    // Replace "source": ... value
    size_t srcPos = cell.find("\"source\"");
    if (srcPos != std::string::npos) {
        size_t colonPos = cell.find(':', srcPos + 8);
        size_t valStart = cell.find_first_not_of(" \t\n\r", colonPos + 1);
        if (valStart != std::string::npos) {
            // Replace the source value with the new one
            // Escape the newSource for JSON
            std::string escapedSource = EscapeJson(newSource);
            if (cell[valStart] == '[') {
                // Array format
                cell.replace(valStart, cell.find(']', valStart) + 1 - valStart, "[\"" + escapedSource + "\"]");
            } else {
                // Simple format
                size_t valEnd = cell.find_first_of(",}", valStart);
                cell.replace(valStart, valEnd - valStart, "\"" + escapedSource + "\"");
            }
        }
    }

    // Update cell_type if changed
    size_t typePos = cell.find("\"cell_type\"");
    if (typePos != std::string::npos) {
        size_t colonPos = cell.find(':', typePos + 11);
        size_t valStart = cell.find_first_not_of(" \t\n\r\"", colonPos + 1);
        if (valStart != std::string::npos) {
            size_t valEnd = cell.find('"', valStart);
            if (valEnd != std::string::npos) {
                std::string oldType = cell.substr(valStart, valEnd - valStart);
                if (oldType != cellType) {
                    cell.replace(valStart, valEnd - valStart, cellType);
                }
            }
        }
    }

    cells[cellIndex] = cell;
    WriteNotebook(filePath, cells, content);
    return "Successfully edited cell " + std::to_string(cellIndex) + " in " + filePath;
}

std::string NotebookEditTool::EscapeJson(const std::string& input) {
    std::string result;
    for (char c : input) {
        if (c == '"') result += "\\\"";
        else if (c == '\\') result += "\\\\";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else result += c;
    }
    return result;
}

std::string NotebookEditTool::CreateCell(const std::string& cellType, const std::string& source) {
    std::string escaped = EscapeJson(source);
    if (cellType == "code") {
        return "    {\n"
               "      \"cell_type\": \"code\",\n"
               "      \"source\": [\""+ escaped + "\"],\n"
               "      \"metadata\": {},\n"
               "      \"outputs\": [],\n"
               "      \"execution_count\": null\n"
               "    }";
    } else {
        return "    {\n"
               "      \"cell_type\": \"" + cellType + "\",\n"
               "      \"source\": [\"" + escaped + "\"],\n"
               "      \"metadata\": {}\n"
               "    }";
    }
}

std::string NotebookEditTool::CreateEmptyNotebook(const std::string& cellType, const std::string& source) {
    return "{\n"
           " \"nbformat\": 4,\n"
           " \"nbformat_minor\": 5,\n"
           " \"metadata\": {\n"
           "  \"kernelspec\": {\"display_name\": \"Python 3\", \"language\": \"python\", \"name\": \"python3\"},\n"
           "  \"language_info\": {\"name\": \"python\"}\n"
           " },\n"
           " \"cells\": [\n" + CreateCell(cellType, source) + "\n"
           " ]\n"
           "}";
}

void NotebookEditTool::ExtractCells(const std::string& content, std::vector<std::string>& cells) {
    size_t cellsStart = content.find("\"cells\"");
    if (cellsStart == std::string::npos) return;
    size_t arrayStart = content.find('[', cellsStart);
    if (arrayStart == std::string::npos) return;

    int depth = 1;
    size_t pos = arrayStart + 1;
    size_t cellStart = std::string::npos;
    int cellDepth = 0;

    while (pos < content.length() && depth > 0) {
        char c = content[pos];
        if (c == '{') {
            if (depth == 1) cellStart = pos;
            cellDepth++;
        } else if (c == '}') {
            cellDepth--;
            if (cellDepth == 0 && cellStart != std::string::npos) {
                cells.push_back(content.substr(cellStart, pos - cellStart + 1));
                cellStart = std::string::npos;
            }
        } else if (c == '[') {
            depth++;
        } else if (c == ']') {
            depth--;
        }
        pos++;
    }
}

void NotebookEditTool::WriteNotebook(const std::string& filePath, const std::vector<std::string>& cells, const std::string& originalContent) {
    size_t cellsStart = originalContent.find("\"cells\"");
    size_t arrayStart = originalContent.find('[', cellsStart);
    if (arrayStart == std::string::npos) return;
    size_t arrayEnd = originalContent.find(']', arrayStart);
    if (arrayEnd == std::string::npos) return;

    std::ostringstream oss;
    oss << originalContent.substr(0, arrayStart + 1) << "\n";
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i > 0) oss << ",\n";
        oss << cells[i];
    }
    oss << "\n  " << originalContent.substr(arrayEnd);

    std::ofstream outFile(filePath, std::ios::out | std::ios::trunc);
    if (outFile.is_open()) {
        outFile << oss.str();
        outFile.close();
    }
}

#include "src/core/dream_processor.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "include/model.h"
#include "src/core/history_store.h"
#include "src/tools/builtin_tools/edit_file_tool.h"
#include "src/tools/builtin_tools/read_file_tool.h"
#include "src/tools/builtin_tools/write_file_tool.h"
#include "src/utils/tool_parser.h"

namespace fs = std::filesystem;

namespace jiuwen {

DreamProcessor::DreamProcessor(DreamConfig config)
    : config_(std::move(config))
{
}

std::string DreamProcessor::LoadFile(const std::string& path) const
{
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string DreamProcessor::TruncateText(const std::string& text, int maxChars) const
{
    if (static_cast<int>(text.length()) <= maxChars) return text;
    return text.substr(0, maxChars) + "\n... (truncated)";
}

std::string DreamProcessor::FindingsToText(const std::vector<DreamFinding>& findings) const
{
    std::stringstream ss;
    for (const auto& f : findings) {
        if (f.type == "SKILL") {
            ss << "[SKILL] " << f.skillName << ": " << f.skillDesc << "\n";
        } else {
            ss << "[" << f.type << "] " << f.content << "\n";
        }
    }
    return ss.str();
}

void DreamProcessor::RefreshFileCache(const std::string& workspace, const std::string& toolName, const std::string& args, std::string& memoryContent, std::string& soulContent, std::string& userContent) const
{
    if (toolName != "read_file" && toolName != "edit_file" && toolName != "write_file") {
        return;
    }

    std::string path;
    try {
        size_t pathKey = args.find("\"path\"");
        if (pathKey != std::string::npos) {
            size_t colon = args.find(':', pathKey + 6);
            size_t valStart = args.find_first_not_of(" \t", colon + 1);
            if (valStart != std::string::npos && args[valStart] == '"') {
                size_t valEnd = args.find('"', valStart + 1);
                if (valEnd != std::string::npos) {
                    path = args.substr(valStart + 1, valEnd - valStart - 1);
                }
            }
        }
    } catch (...) {
        return;
    }

    if (path.empty()) return;

    if (path.find("MEMORY.md") != std::string::npos) {
        memoryContent = LoadFile(workspace + "/memory/MEMORY.md");
    } else if (path.find("SOUL.md") != std::string::npos) {
        soulContent = LoadFile(workspace + "/SOUL.md");
    } else if (path.find("USER.md") != std::string::npos) {
        userContent = LoadFile(workspace + "/USER.md");
    }
}

std::string DreamProcessor::LoadDreamPhase1Prompt() const
{
    std::string path = config_.dataBasePath + "/dream_phase1.md";
    std::string content = LoadFile(path);
    if (!content.empty()) return content;

    std::string p1;
    p1 += "You have TWO equally important tasks:\n";
    p1 += "1. Extract new facts from conversation history\n";
    p1 += "2. Deduplicate existing memory files - find and flag redundant, overlapping, or stale content\n\n";
    p1 += "Output one line per finding:\n";
    p1 += "[MEMORY] atomic fact (not already in memory)\n";
    p1 += "[MEMORY-REMOVE] reason for removal\n";
    p1 += "[SOUL] bot behavior/tone correction\n";
    p1 += "[SOUL-REMOVE] reason for removal\n";
    p1 += "[USER] user identity, preference correction\n";
    p1 += "[USER-REMOVE] reason for removal\n";
    p1 += "[SKILL] kebab-case-name: one-line description of a reusable pattern\n";
    p1 += "[SKIP] if nothing new\n\n";
    p1 += "Rules:\n";
    p1 += "- Atomic facts: has a cat named Luna not discussed pet care\n";
    p1 += "- Corrections: [USER] location is Tokyo, not Osaka\n";
    p1 += "- Capture confirmed approaches the user validated\n\n";
    p1 += "Staleness check:\n";
    p1 += "- User habits/preferences/personality traits are permanent regardless of age\n";
    p1 += "- Only prune objectively outdated: passed events, resolved tracking, superseded approaches\n\n";
    p1 += "Skill discovery - flag [SKILL] when ALL of these are true:\n";
    p1 += "- A specific, repeatable workflow appeared 2+ times\n";
    p1 += "- It involves clear steps\n";
    p1 += "- It is substantial enough to warrant its own instruction set\n\n";
    p1 += "Do not add: current weather, transient status, temporary errors, conversational filler.\n";
    return p1;
}

std::string DreamProcessor::LoadDreamPhase2Prompt() const
{
    std::string path = config_.dataBasePath + "/dream_phase2.md";
    std::string content = LoadFile(path);
    if (!content.empty()) return content;

    std::string p2;
    p2 += "You are a memory maintenance agent. Your job is to update memory files based on the findings below.\n\n";
    p2 += "## Available Tools\n";
    p2 += "1. read_file: Read a file's content. Use it if you need to see the full content of a file before editing.\n";
    p2 += "   Input: {\"path\": \"path/to/file.md\"}\n";
    p2 += "2. edit_file: Make surgical edits to a file using find-and-replace.\n";
    p2 += "   Input: {\"path\": \"path/to/file.md\", \"old_text\": \"text to find\", \"new_text\": \"replacement text\"}\n";
    p2 += "3. write_file: Create a new file. Only use this for creating skill files under my_skills/.\n";
    p2 += "   Input: {\"path\": \"path/to/file.md\", \"content\": \"file content\"}\n\n";
    p2 += "## File Paths\n";
    p2 += "- MEMORY.md: " + config_.dataBasePath + "/memory/MEMORY.md\n";
    p2 += "- SOUL.md: " + config_.dataBasePath + "/SOUL.md\n";
    p2 += "- USER.md: " + config_.dataBasePath + "/USER.md\n";
    p2 += "- Skills: my_skills/<name>/SKILL.md\n\n";
    p2 += "## Editing Rules\n";
    p2 += "- Use edit_file for existing files, write_file only for new skill files\n";
    p2 += "- For edit_file: use exact text from the file content as old_text\n";
    p2 += "- If old_text doesn't match, use read_file first to get the exact content\n";
    p2 += "- For deletions: use edit_file with new_text as empty string\n";
    p2 += "- Batch multiple edits to the same file into separate edit_file calls\n";
    p2 += "- Surgical edits only - never rewrite entire files\n";
    p2 += "- When done with all updates, output: DONE\n\n";
    p2 += "## Skill Creation Rules\n";
    p2 += "- Use write_file to create my_skills/<name>/SKILL.md\n";
    p2 += "- Include YAML frontmatter with name and description\n";
    p2 += "- Keep SKILL.md under 2000 words\n";
    p2 += "- Include: when to use, steps, output format, example\n";
    p2 += "- Do NOT overwrite existing skills\n";
    return p2;
}

std::vector<DreamFinding> DreamProcessor::Phase1Analysis(
    Model* model,
    const std::string& historyText,
    const std::string& memoryContent,
    const std::string& soulContent,
    const std::string& userContent) const
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream dateStream;
    dateStream << std::put_time(std::gmtime(&time), "%Y-%m-%d");

    std::string fileContext =
        "## Current Date\n" + dateStream.str() + "\n\n"
        "## Current MEMORY.md (" + std::to_string(memoryContent.length()) + " chars)\n" + TruncateText(memoryContent.empty() ? "(empty)" : memoryContent, config_.memoryFileMaxChars) + "\n\n"
        "## Current SOUL.md (" + std::to_string(soulContent.length()) + " chars)\n" + TruncateText(soulContent.empty() ? "(empty)" : soulContent, config_.memoryFileMaxChars) + "\n\n"
        "## Current USER.md (" + std::to_string(userContent.length()) + " chars)\n" + TruncateText(userContent.empty() ? "(empty)" : userContent, config_.memoryFileMaxChars);

    std::string prompt =
        "## Conversation History\n" + historyText + "\n\n" + fileContext;

    std::string systemPrompt = LoadDreamPhase1Prompt();

    Message userMsg;
    userMsg.role = "user";
    userMsg.content = prompt;
    std::string formatted = model->Format(systemPrompt, {userMsg}, {});
    std::string response = model->Invoke(formatted, nullptr).content;

    std::vector<DreamFinding> findings;
    std::istringstream stream(response);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;

        DreamFinding f;
        if (line.rfind("[MEMORY-REMOVE]", 0) == 0) {
            f.type = "MEMORY-REMOVE";
            f.content = line.substr(15);
        } else if (line.rfind("[SOUL-REMOVE]", 0) == 0) {
            f.type = "SOUL-REMOVE";
            f.content = line.substr(13);
        } else if (line.rfind("[USER-REMOVE]", 0) == 0) {
            f.type = "USER-REMOVE";
            f.content = line.substr(13);
        } else if (line.rfind("[MEMORY]", 0) == 0) {
            f.type = "MEMORY";
            f.content = line.substr(8);
        } else if (line.rfind("[SOUL]", 0) == 0) {
            f.type = "SOUL";
            f.content = line.substr(6);
        } else if (line.rfind("[USER]", 0) == 0) {
            f.type = "USER";
            f.content = line.substr(6);
        } else if (line.rfind("[SKILL]", 0) == 0) {
            f.type = "SKILL";
            std::string skillPart = line.substr(7);
            size_t colonPos = skillPart.find(':');
            if (colonPos != std::string::npos) {
                f.skillName = skillPart.substr(0, colonPos);
                f.skillDesc = skillPart.substr(colonPos + 1);
            } else {
                f.skillName = skillPart;
            }
        } else if (line.rfind("[SKIP]", 0) == 0) {
            continue;
        } else {
            continue;
        }

        if (!f.content.empty() || f.type == "SKILL") {
            findings.push_back(f);
        }
    }

    return findings;
}

std::string DreamProcessor::BuildPhase2Prompt(
    const std::vector<DreamFinding>& findings,
    const std::string& scratchpad,
    const std::string& memoryContent,
    const std::string& soulContent,
    const std::string& userContent) const
{
    std::string prompt;
    prompt += "## Findings to Apply\n" + FindingsToText(findings) + "\n\n";
    prompt += "## Current File Contents\n\n";
    prompt += "### " + config_.dataBasePath + "/memory/MEMORY.md (" + std::to_string(memoryContent.length()) + " chars)\n";
    prompt += TruncateText(memoryContent.empty() ? "(empty)" : memoryContent, config_.memoryFileMaxChars) + "\n\n";
    prompt += "### " + config_.dataBasePath + "/SOUL.md (" + std::to_string(soulContent.length()) + " chars)\n";
    prompt += TruncateText(soulContent.empty() ? "(empty)" : soulContent, config_.memoryFileMaxChars) + "\n\n";
    prompt += "### " + config_.dataBasePath + "/USER.md (" + std::to_string(userContent.length()) + " chars)\n";
    prompt += TruncateText(userContent.empty() ? "(empty)" : userContent, config_.memoryFileMaxChars);

    if (!scratchpad.empty()) {
        prompt += "\n\n## Tool Interaction History\n" + scratchpad;
    }

    return prompt;
}

std::string DreamProcessor::ExecuteTool(const std::string& toolName, const std::string& input) const
{
    try {
        if (toolName == "read_file") {
            ReadFileTool tool;
            return tool.Invoke(input);
        } else if (toolName == "write_file") {
            WriteFileTool tool;
            return tool.Invoke(input);
        } else if (toolName == "edit_file") {
            EditFileTool tool;
            return tool.Invoke(input);
        } else {
            return "Error: Unknown tool '" + toolName + "'";
        }
    } catch (const std::exception& e) {
        return "Error: " + std::string(e.what());
    }
}

bool DreamProcessor::Phase2Execution(
    Model* model,
    const std::vector<DreamFinding>& findings) const
{
    std::string workspace = config_.dataBasePath;
    std::string memoryContent = LoadFile(workspace + "/memory/MEMORY.md");
    std::string soulContent = LoadFile(workspace + "/SOUL.md");
    std::string userContent = LoadFile(workspace + "/USER.md");

    std::string scratchpad;
    std::string systemPrompt = LoadDreamPhase2Prompt();

    for (int iteration = 0; iteration < config_.maxIterations; ++iteration) {
        std::string prompt = BuildPhase2Prompt(findings, scratchpad, memoryContent, soulContent, userContent);
        Message userMsg;
        userMsg.role = "user";
        userMsg.content = prompt;
        std::string formatted = model->Format(systemPrompt, {userMsg}, {});
        std::string response = model->Invoke(formatted, nullptr).content;

        if (response.find("DONE") != std::string::npos) {
            return true;
        }

        std::vector<ParsedToolCall> toolCalls = ExtractAllToolCalls(response);
        if (toolCalls.empty()) {
            return true;
        }

        for (const auto& call : toolCalls) {
            std::string observation = ExecuteTool(call.name, call.arguments);

            std::stringstream entry;
            entry << "Thought: (model response iteration " << (iteration + 1) << ")\n";
            entry << "Action: " << call.name << "\n";
            entry << "Action Input: " << call.arguments << "\n";
            entry << "Observation: " << observation << "\n\n";
            scratchpad += entry.str();

            RefreshFileCache(workspace, call.name, call.arguments, memoryContent, soulContent, userContent);
        }
    }

    return false;
}

bool DreamProcessor::Run(Model* model, HistoryStore* historyStore)
{
    if (!model || !historyStore) return false;

    int lastCursor = historyStore->GetLastDreamCursor();
    std::vector<HistoryEntry> entries = historyStore->ReadUnprocessedHistory(lastCursor);

    if (entries.empty()) {
        return false;
    }

    int batchSize = std::min(static_cast<int>(entries.size()), config_.maxBatchSize);
    auto batchEnd = entries.begin() + batchSize;

    std::stringstream historyText;
    for (auto it = entries.begin(); it != batchEnd; ++it) {
        historyText << "[" << it->timestamp << "] " << it->role << ": "
                    << TruncateText(it->content, config_.historyEntryPreviewMaxChars) << "\n";
    }

    std::string workspace = config_.dataBasePath;
    std::string memoryContent = LoadFile(workspace + "/memory/MEMORY.md");
    std::string soulContent = LoadFile(workspace + "/SOUL.md");
    std::string userContent = LoadFile(workspace + "/USER.md");

    std::vector<DreamFinding> findings = Phase1Analysis(
        model, historyText.str(), memoryContent, soulContent, userContent);

    if (findings.empty()) {
        int newCursor = (batchEnd - 1)->cursor;
        historyStore->SetLastDreamCursor(newCursor);
        return false;
    }

    bool success = Phase2Execution(model, findings);

    int newCursor = (batchEnd - 1)->cursor;
    historyStore->SetLastDreamCursor(newCursor);

    return success;
}

} // namespace jiuwen

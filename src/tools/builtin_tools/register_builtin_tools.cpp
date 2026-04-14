#include "builtin_tools.h"
#include "resource_manager.h"
#include "time_info_tool.h"
#include "web_search_tool.h"
#include "web_fetch_tool.h"
#include "read_file_tool.h"
#include "write_file_tool.h"
#include "edit_file_tool.h"
#include "list_dir_tool.h"
#include "glob_tool.h"
#include "grep_tool.h"
#include "exec_tool.h"
#include "notebook_edit_tool.h"
#include "file_state_tool.h"

void RegisterBuiltinTools() {
    auto& rm = ResourceManager::GetInstance();

    rm.RegisterTool("time_info", []() { return std::make_unique<TimeInfoTool>(); });
    rm.RegisterTool("web_search", []() { return std::make_unique<WebSearchTool>(); });
    rm.RegisterTool("web_fetcher", []() { return std::make_unique<WebFetcherTool>(); });
    rm.RegisterTool("read_file", []() { return std::make_unique<ReadFileTool>(); });
    rm.RegisterTool("write_file", []() { return std::make_unique<WriteFileTool>(); });
    rm.RegisterTool("edit_file", []() { return std::make_unique<EditFileTool>(); });
    rm.RegisterTool("list_dir", []() { return std::make_unique<ListDirTool>(); });
    rm.RegisterTool("glob", []() { return std::make_unique<GlobTool>(); });
    rm.RegisterTool("grep", []() { return std::make_unique<GrepTool>(); });
    rm.RegisterTool("exec", []() { return std::make_unique<ExecTool>(); });
    rm.RegisterTool("notebook_edit", []() { return std::make_unique<NotebookEditTool>(); });
    rm.RegisterTool("file_state", []() { return std::make_unique<FileStateTool>(); });
}

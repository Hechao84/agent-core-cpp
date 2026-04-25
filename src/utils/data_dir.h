#pragma once

#include <string>

#ifdef _WIN32
  #ifdef BUILDING_AGENT_FRAMEWORK
    #define DATA_DIR_API __declspec(dllexport)
  #else
    #define DATA_DIR_API __declspec(dllimport)
  #endif
#else
  #define DATA_DIR_API
#endif

namespace jiuwen {

// DataDir provides a centralized way to manage agent data directories.
// All agent-generated data (context, cron, memory, etc.) is stored under
// a base directory, with different data types in separate subdirectories.
//
// Directory structure:
//   data/
//   ├── context/     - Session context and conversation history
//   ├── cron/        - Scheduled reminders and job definitions
//   -- MEMORY.md     - Long-term memory file
//   -- HEARTBEAT.md  - Heartbeat file
//   -- temp/         - Temporary files and caches
class DATA_DIR_API DataDir {
public:
    DataDir();
    explicit DataDir(const std::string& basePath);

    // Set/Get the base data directory
    void SetBasePath(const std::string& path);
    std::string GetBasePath() const;

    // Get subdirectory path for a specific data type.
    // Creates the directory if it doesn't exist.
    std::string GetPath(const std::string& dataType) const;

    // Get full file path within a data type subdirectory.
    std::string GetFilePath(const std::string& dataType, const std::string& fileName) const;

private:
    std::string basePath_;

    void EnsureDirectory(const std::string& path) const;
};

// Get the global DataDir singleton instance
DATA_DIR_API DataDir& GetDataDir();

// Initialize the global DataDir with a custom base path
DATA_DIR_API void InitDataDir(const std::string& basePath);

} // namespace jiuwen

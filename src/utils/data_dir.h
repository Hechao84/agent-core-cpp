#pragma once

#include <mutex>
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
// All agent-generated data (context, cron, memory, sessions, etc.) is stored under
// a base directory, with different data types in separate subdirectories.
//
// Directory structure (multi-session):
//   data/
//   ├── HEARTBEAT.md       - Shared periodic task definitions
//   ├── MEMORY.md          - Shared long-term memory (cross-session)
//   ├── cron/              - Cron job definitions (shared)
//   │   └── jobs.json
//   ├── sessions/          - Per-session data
//   │   ├── __DEFAULT__/
//   │   │   └── history.json
//   │   ├── <session_id>/
//   │   │   └── history.json
//   │   ├── __HEARTBEAT__/
//   │   │   └── history.json
//   │   └── __CRON__/
//   │       └── history.json
//   └── temp/              - Temporary files and caches
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

    // Get session-specific data path: data/sessions/<sessionId>/<subPath>
    std::string GetSessionPath(const std::string& sessionId, const std::string& subPath = "") const;

    // Get full file path within a session directory.
    std::string GetSessionFilePath(const std::string& sessionId, const std::string& fileName) const;

    // Get shared memory file path: data/MEMORY.md
    std::string GetMemoryPath() const;

    // Get shared heartbeat file path: data/HEARTBEAT.md
    std::string GetHeartbeatPath() const;

    // Get shared cron jobs file path: data/cron/jobs.json
    std::string GetCronJobsPath() const;

private:
    mutable std::mutex mutex_;
    std::string basePath_;

    void EnsureDirectory(const std::string& path) const;
};

// Get the global DataDir singleton instance (thread-safe)
DATA_DIR_API DataDir& GetDataDir();

// Initialize the global DataDir with a custom base path (thread-safe)
DATA_DIR_API void InitDataDir(const std::string& basePath);

} // namespace jiuwen

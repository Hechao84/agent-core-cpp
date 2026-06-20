
// Test declaration macros - tests are registered via static constructors
// in the individual test files. This main just runs them all.

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include "test_runner.h"

namespace fs = std::filesystem;

#if defined(_WIN32)
    #include <direct.h>
    #define chdir _chdir
#else
    #include <unistd.h>
#endif

static void CleanupTempDirs()
{
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(".", ec)) {
        if (entry.is_directory() && entry.path().filename().string().rfind("test_tmp_", 0) == 0) {
            fs::remove_all(entry.path(), ec);
        }
    }
}

int main()
{
    // Change to project root so ./data paths resolve correctly
    fs::path buildDir = fs::current_path();
    fs::path projectRoot = buildDir.parent_path();
    if (fs::exists(projectRoot / "CMakeLists.txt")) {
        chdir(projectRoot.string().c_str());
    }

    // Ensure data directory structure exists
    fs::create_directories("./data/memory");
    fs::create_directories("./data/sessions");
    fs::create_directories("./data/cron");
    fs::create_directories("./data/context");

    // Clean up any leftover temp dirs from previous runs
    CleanupTempDirs();

    std::cout << "Running jiuwen-lite Unit Tests...\n";
    std::cout << "================================\n";
    int result = RunAllTests();

    // Clean up temp directories created by tests
    CleanupTempDirs();

    return result;
}

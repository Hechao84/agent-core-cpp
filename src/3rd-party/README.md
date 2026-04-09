# 3rd-party Dependencies

This directory is reserved for third-party library source code used by the project.

## How it works
Currently, all third-party dependencies (such as `curl` and `nlohmann/json`) are **automatically downloaded** and built via CMake `FetchContent` during the compilation process.

**You do not need to manually add or extract files into this folder.**

## Adding New Dependencies
If you need to add a new dependency locally for testing:
1. Place the source files here.
2. Ensure `CMakeLists.txt` is configured to use them.
3. **Do not commit third-party source code to the repository** (it is ignored by `.gitignore` to keep the repository lightweight).

cmake_minimum_required(VERSION 3.15)
project(jiuwen-lite LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fix MSVC PDB locking issue (Use /Z7 to embed debug info in .obj instead of separate .pdb)
if(MSVC)
    string(APPEND CMAKE_CXX_FLAGS " /FS")
    string(APPEND CMAKE_C_FLAGS " /FS")
    # Fallback/Alternative to /FS is using /Z7
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

# Dependencies
include(FetchContent)

# 1. nlohmann/json
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_GetProperties(json)
if(NOT json_POPULATED)
    FetchContent_Populate(json)
    add_subdirectory(${json_SOURCE_DIR} ${json_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

# 2. curl
option(USE_LOCAL_CURL "Use local curl source from src/3rd-party" OFF)

if(USE_LOCAL_CURL)
    message(STATUS "Using local curl source from 3rd-party")
    set(CURL_LOCAL_DIR ${CMAKE_SOURCE_DIR}/src/3rd-party/curl-8.7.1)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build curl as static lib" FORCE)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "Use Windows native SSL" FORCE)
    set(CURL_ZLIB OFF CACHE BOOL "Disable ZLIB" FORCE)
    set(HTTP_ONLY ON CACHE BOOL "Build HTTP only" FORCE)
    add_subdirectory(${CURL_LOCAL_DIR} ${CMAKE_BINARY_DIR}/curl-build EXCLUDE_FROM_ALL)
    set(CURL_LIB libcurl)
    target_include_directories(jiuwen-lite PRIVATE ${CURL_LOCAL_DIR}/include)
else()
    find_package(CURL QUIET)
    if(CURL_FOUND)
        message(STATUS "Using system curl")
        set(CURL_LIB CURL::libcurl)
    else()
        message(STATUS "System curl not found, fetching via FetchContent")
        FetchContent_Declare(
            curl
            GIT_REPOSITORY https://github.com/curl/curl.git
            GIT_TAG curl-8_7_1
        )
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build curl as static lib" FORCE)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "Use Windows native SSL" FORCE)
        set(CURL_ZLIB OFF CACHE BOOL "Disable ZLIB" FORCE)
        set(HTTP_ONLY ON CACHE BOOL "Build HTTP only" FORCE)
        FetchContent_MakeAvailable(curl)
        set(CURL_LIB libcurl)
    endif()
endif()

# Define Shared Library (agent_framework)
add_library(agent_framework SHARED
    src/core/agent.cpp
    src/core/agent_worker.cpp
    src/context_engine/context_engine.cpp
    src/context_engine/md_storage.cpp
    src/context_engine/db_storage.cpp
    src/workers/agent_worker_factory.cpp
    src/workers/react_worker.cpp
    src/workers/plan_execute_worker.cpp
    src/workers/workflow_worker.cpp
    src/resource_manager/resource_manager.cpp
    src/tools/tool.cpp
    src/tools/tool_selector.cpp
    src/tools/mcp_tool.cpp
    src/models/model.cpp
    src/models/openai_model.cpp
    src/models/anthropic_model.cpp
)

target_include_directories(agent_framework PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(agent_framework PRIVATE BUILDING_AGENT_FRAMEWORK)

# 3. SQLite3 (For Database Context Storage)
option(USE_LOCAL_SQLITE "Use local sqlite3 source from src/3rd-party/sqlite" OFF)

if(USE_LOCAL_SQLITE)
    message(STATUS "Using local sqlite3 from src/3rd-party")
    add_library(sqlite3 STATIC ${CMAKE_SOURCE_DIR}/src/3rd-party/sqlite/sqlite3.c)
    target_include_directories(sqlite3 PUBLIC ${CMAKE_SOURCE_DIR}/src/3rd-party/sqlite)
    set(SQLITE_LIB sqlite3)
else()
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND)
        message(STATUS "Using system sqlite3")
        set(SQLITE_LIB SQLite::SQLite3)
    else()
        message(STATUS "SQLite3 not found locally or system-wide, fetching via FetchContent...")
        FetchContent_Declare(
            sqlite3
            URL https://www.sqlite.org/2024/sqlite-amalgamation-3450300.zip
        )
        FetchContent_GetProperties(sqlite3)
        if(NOT sqlite3_POPULATED)
            FetchContent_Populate(sqlite3)
            add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
            target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
        endif()
        set(SQLITE_LIB sqlite3)
    endif()
endif()

target_link_libraries(agent_framework PRIVATE 
    ${CURL_LIB}
    ${SQLITE_LIB}
    nlohmann_json::nlohmann_json
)

# Define Executable (jiuwen-lite)
add_executable(jiuwen-lite src/main.cpp)

target_include_directories(jiuwen-lite PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(jiuwen-lite PRIVATE agent_framework)

# Install rules
install(TARGETS agent_framework jiuwen-lite
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)

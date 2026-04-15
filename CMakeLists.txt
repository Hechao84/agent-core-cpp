cmake_minimum_required(VERSION 3.15)
project(jiuwen-lite LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform-specific Compiler Flags
if(MSVC)
    string(APPEND CMAKE_CXX_FLAGS " /FS")
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -fPIC)
endif()

# All third-party pre-built libs
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/3rd-party")
set(TP_LIBS_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/libs")
set(TP_INCLUDE_DIR  "${THIRD_PARTY_DIR}/include")
set(TP_SRC_DIR      "${THIRD_PARTY_DIR}/src")

# ============================================================
# 1. Check if pre-built libraries exist
# ============================================================
include(FindPackageHandleStandardArgs)

set(USE_PREBUILT_THIRD_PARTY OFF)
if(EXISTS "${TP_INCLUDE_DIR}/nlohmann/json.hpp")
    set(USE_PREBUILT_THIRD_PARTY ON)
    message(STATUS "Using pre-built third-party includes from src/3rd-party/")
endif()

# System/system dependencies
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
set(CURL_LIB CURL::libcurl)

# ============================================================
# 2. SQLite3 (Pre-built shared library)
# ============================================================
set(SQLITE_LIB sqlite3)

if(EXISTS "${TP_LIBS_DIR}/libsqlite3.so")
    message(STATUS "Using pre-built sqlite3 from libs/")
    add_library(sqlite3 SHARED IMPORTED GLOBAL)
    set_target_properties(sqlite3 PROPERTIES
        IMPORTED_LOCATION "${TP_LIBS_DIR}/libsqlite3.so"
        INTERFACE_INCLUDE_DIRECTORIES "${TP_INCLUDE_DIR}"
    )
else()
    set(LOCAL_SQLITE_SRC "${TP_SRC_DIR}/sqlite3-src/sqlite3.c")
    if(EXISTS "${LOCAL_SQLITE_SRC}")
        message(STATUS "Using local sqlite3 source")
        add_library(sqlite3 SHARED ${LOCAL_SQLITE_SRC})
        target_include_directories(sqlite3 PUBLIC "${TP_SRC_DIR}/sqlite3-src" ${TP_INCLUDE_DIR})
        target_link_libraries(sqlite3 PUBLIC dl pthread)
    else()
        message(STATUS "Warning: sqlite3 not found in libs/ or src/. Building without sqlite.")
        add_library(sqlite3 INTERFACE IMPORTED)
    endif()
endif()

# ============================================================
# 3. nlohmann/json
# ============================================================
if(NOT TARGET nlohmann_json::nlohmann_json)
    add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED GLOBAL)
    set_property(TARGET nlohmann_json::nlohmann_json PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${TP_INCLUDE_DIR}")
endif()

# ============================================================
# Shared Library: agent_framework
# ============================================================
add_library(agent_framework SHARED
    src/protocol/mcp_client.cpp
    src/core/agent.cpp
    src/core/agent_worker.cpp
    src/skills/skill_engine.cpp
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
    src/tools/builtin_tools/time_info_tool.cpp
    src/tools/builtin_tools/web_search_tool.cpp
    src/tools/builtin_tools/web_fetch_tool.cpp
    src/tools/builtin_tools/read_file_tool.cpp
    src/tools/builtin_tools/write_file_tool.cpp
    src/tools/builtin_tools/edit_file_tool.cpp
    src/tools/builtin_tools/list_dir_tool.cpp
    src/tools/builtin_tools/glob_tool.cpp
    src/tools/builtin_tools/grep_tool.cpp
    src/tools/builtin_tools/exec_tool.cpp
    src/tools/builtin_tools/notebook_edit_tool.cpp
    src/tools/builtin_tools/file_state_tool.cpp
    src/models/model.cpp
    src/models/openai_model.cpp
    src/models/anthropic_model.cpp
)

target_include_directories(agent_framework PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(agent_framework PRIVATE BUILDING_AGENT_FRAMEWORK)

target_include_directories(agent_framework PRIVATE ${CURL_INCLUDE_DIRS})

target_link_libraries(agent_framework PRIVATE 
    ${CURL_LIB}
    ${SQLITE_LIB}
    nlohmann_json::nlohmann_json
    pthread
    dl
)

# ============================================================
# Executables
# ============================================================
add_executable(jiuwen-lite src/main.cpp)
target_include_directories(jiuwen-lite PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(jiuwen-lite PRIVATE agent_framework nlohmann_json::nlohmann_json)

# Optional test targets
add_executable(unittest unittest/main.cpp unittest/test_types.cpp unittest/test_tool.cpp)
target_include_directories(unittest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/unittest)
target_link_libraries(unittest PRIVATE agent_framework nlohmann_json::nlohmann_json)

install(TARGETS agent_framework jiuwen-lite
    ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)

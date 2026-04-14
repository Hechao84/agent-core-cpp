cmake_minimum_required(VERSION 3.15)
project(jiuwen-lite LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform-specific Compiler Flags
if(MSVC)
    string(APPEND CMAKE_CXX_FLAGS " /FS")
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -fPIC) # -fPIC is required for shared libraries on Linux/Android/OH
endif()

# std::filesystem compatibility for older GCC (<9)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    find_library(STDCXXFS_LIB stdc++fs QUIET)
    if(STDCXXFS_LIB)
        list(APPEND PLATFORM_LIBS ${STDCXXFS_LIB})
    endif()
endif()

# Dependencies
include(FetchContent)

# 1. curl
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
        
        # Platform-specific SSL backend for curl
        if(WIN32)
            set(CURL_USE_SCHANNEL ON CACHE BOOL "Use Windows native SSL" FORCE)
            set(CURL_USE_OPENSSL OFF CACHE BOOL "Use OpenSSL" FORCE)
        else()
            set(CURL_USE_SCHANNEL OFF CACHE BOOL "Use Schannel" FORCE)
            set(CURL_USE_OPENSSL ON CACHE BOOL "Use OpenSSL" FORCE)
        endif()
        
        set(CURL_ZLIB OFF CACHE BOOL "Disable ZLIB" FORCE)
        set(HTTP_ONLY ON CACHE BOOL "Build HTTP only" FORCE)
        set(CURL_DISABLE_TESTS ON CACHE BOOL "Disable curl tests for cross-compilation" FORCE)
        FetchContent_MakeAvailable(curl)
        set(CURL_LIB libcurl)
    endif()
endif()

# 2. MCP SDK (Model Context Protocol Client) - Must be before agent_framework to provide json
FetchContent_Declare(
    agent_protocol
    GIT_REPOSITORY https://github.com/openJiuwen-ai/agent-protocol.git
    GIT_TAG        main
)
FetchContent_GetProperties(agent_protocol)
if(NOT agent_protocol_POPULATED)
    message(STATUS "Fetching OpenJiuwen Agent Protocol (MCP SDK)...")
    FetchContent_Populate(agent_protocol)
    
    set(MCP_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(MCP_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(MCP_ENABLE_COVERAGE OFF CACHE BOOL "" FORCE)

    # Make SDK's find_package calls use our already resolved dependencies
    set(CURL_FOUND TRUE)
    set(CURL_LIBRARY ${CURL_LIB})
    set(CURL_INCLUDE_DIR ${curl_BINARY_DIR}/include)

    # Find OpenSSL for the SDK
    find_package(OpenSSL REQUIRED)

    add_subdirectory(${agent_protocol_SOURCE_DIR}/MCP/cpp-sdk ${agent_protocol_BINARY_DIR}/MCP_cpp_build)
    
    # Fix SDK's hardcoded CMAKE_SOURCE_DIR which points to our root project
    if(TARGET mcp)
        target_include_directories(mcp PUBLIC 
            ${agent_protocol_SOURCE_DIR}/MCP/cpp-sdk/include
        )
        target_include_directories(mcp PRIVATE
            ${agent_protocol_BINARY_DIR}/MCP_cpp_build/include
        )
    endif()

    message(STATUS "MCP SDK integrated")
endif()

# Define Shared Library (agent_framework)
add_library(agent_framework SHARED
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

if(TARGET mcp)
    target_link_libraries(agent_framework PRIVATE mcp pthread dl)
    target_include_directories(agent_framework PRIVATE 
        ${agent_protocol_SOURCE_DIR}/MCP/cpp-sdk/include
        ${CURL_INCLUDE_DIR}
    )
endif()

target_link_libraries(agent_framework PRIVATE 
    ${CURL_LIB}
    ${SQLITE_LIB}
    ${PLATFORM_LIBS}
    nlohmann_json::nlohmann_json
)

# Define Executable (jiuwen-lite)
add_executable(jiuwen-lite src/main.cpp)

target_include_directories(jiuwen-lite PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(jiuwen-lite PRIVATE agent_framework nlohmann_json::nlohmann_json)

# Define Testcase Executable
add_executable(test_builtin_tools testcases/test_builtin_tools.cpp)
target_include_directories(test_builtin_tools PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/builtin_tools
)
target_link_libraries(test_builtin_tools PRIVATE agent_framework nlohmann_json::nlohmann_json)

# Unit Tests (src/unittest/)
add_executable(unittest
    unittest/main.cpp
    unittest/test_types.cpp
    unittest/test_tool.cpp
    unittest/test_file_tools.cpp
    unittest/test_search_tools.cpp
    unittest/test_misc_tools.cpp
    unittest/test_tool_selector.cpp
    unittest/test_context_engine.cpp
    unittest/test_resource_manager.cpp
    unittest/test_agent_factory.cpp
    unittest/test_integration.cpp
)
target_include_directories(unittest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/unittest
)
target_link_libraries(unittest PRIVATE agent_framework nlohmann_json::nlohmann_json)

# Functional Tests (testcases/)
add_executable(test_functional_tools testcases/test_functional_tools.cpp)
target_include_directories(test_functional_tools PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/builtin_tools
)
target_link_libraries(test_functional_tools PRIVATE agent_framework nlohmann_json::nlohmann_json)

# Install rules
install(TARGETS agent_framework jiuwen-lite
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)

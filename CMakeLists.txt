cmake_minimum_required(VERSION 3.15)
project(jiuwen-lite LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform-specific Compiler Flags
if(MSVC)
    string(APPEND CMAKE_CXX_FLAGS " /FS /utf-8")
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -fPIC)
endif()

# All third-party pre-built libs
set(THIRD_PARTY_DIR          "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(TRIRD_PARTY_LIBS_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/libs")
set(TRIRD_PARTY_INCLUDE_DIR  "${THIRD_PARTY_DIR}/include")
set(TRIRD_PARTY_SRC_DIR      "${THIRD_PARTY_DIR}/src")

# ============================================================
# 1. Check if pre-built libraries exist
# ============================================================
include(FindPackageHandleStandardArgs)

set(USE_PREBUILT_THIRD_PARTY OFF)
if(EXISTS "${TRIRD_PARTY_INCLUDE_DIR}/nlohmann/json.hpp")
    set(USE_PREBUILT_THIRD_PARTY ON)
    message(STATUS "Using pre-built third-party includes from third_party/")
endif()

# System/system dependencies
find_package(CURL REQUIRED)
set(CURL_LIB CURL::libcurl)

# OpenSSL (required for httplib WebSocket wss:// support)
find_package(OpenSSL)
if(OpenSSL_FOUND)
    message(STATUS "OpenSSL ${OpenSSL_VERSION} found, enabling httplib WebSocket TLS support")
    add_compile_definitions(CPPHTTPLIB_OPENSSL_SUPPORT)
else()
    message(WARNING "OpenSSL not found. httplib wss:// WebSocket support will be disabled.")
endif()

# ============================================================
# 2. SQLite3 (Pre-built shared library)
# ============================================================
set(SQLITE_LIB sqlite3)

if(WIN32)
    if(EXISTS "${TRIRD_PARTY_LIBS_DIR}/sqlite3.dll")
        message(STATUS "Using pre-built sqlite3.dll from libs/")
        add_library(sqlite3 SHARED IMPORTED GLOBAL)
        set_target_properties(sqlite3 PROPERTIES
            IMPORTED_LOCATION "${TRIRD_PARTY_LIBS_DIR}/sqlite3.dll"
            INTERFACE_INCLUDE_DIRECTORIES "${TRIRD_PARTY_INCLUDE_DIR}"
        )
        if(EXISTS "${TRIRD_PARTY_LIBS_DIR}/sqlite3.lib")
            set_target_properties(sqlite3 PROPERTIES
                IMPORTED_IMPLIB "${TRIRD_PARTY_LIBS_DIR}/sqlite3.lib"
            )
        endif()
    else()
        set(LOCAL_SQLITE_SRC "${TRIRD_PARTY_SRC_DIR}/sqlite3-src/sqlite3.c")
        if(EXISTS "${LOCAL_SQLITE_SRC}")
            message(STATUS "Building sqlite3 from source")
            add_library(sqlite3 SHARED ${LOCAL_SQLITE_SRC})
            target_include_directories(sqlite3 PUBLIC "${TRIRD_PARTY_SRC_DIR}/sqlite3-src" ${TRIRD_PARTY_INCLUDE_DIR})
            target_compile_definitions(sqlite3 PRIVATE SQLITE_API=__declspec(dllexport))
        else()
            message(STATUS "Warning: sqlite3 not found. Building without SQLite support.")
            add_library(sqlite3 INTERFACE IMPORTED)
        endif()
    endif()
else()
    if(EXISTS "${TRIRD_PARTY_LIBS_DIR}/libsqlite3.so")
        message(STATUS "Using pre-built sqlite3 from libs/")
        add_library(sqlite3 SHARED IMPORTED GLOBAL)
        set_target_properties(sqlite3 PROPERTIES
            IMPORTED_LOCATION "${TRIRD_PARTY_LIBS_DIR}/libsqlite3.so"
            INTERFACE_INCLUDE_DIRECTORIES "${TRIRD_PARTY_INCLUDE_DIR}"
        )
    else()
        set(LOCAL_SQLITE_SRC "${TRIRD_PARTY_SRC_DIR}/sqlite3-src/sqlite3.c")
        if(EXISTS "${LOCAL_SQLITE_SRC}")
            message(STATUS "Using local sqlite3 source")
            add_library(sqlite3 SHARED ${LOCAL_SQLITE_SRC})
            target_include_directories(sqlite3 PUBLIC "${TRIRD_PARTY_SRC_DIR}/sqlite3-src" ${TRIRD_PARTY_INCLUDE_DIR})
            target_link_libraries(sqlite3 PUBLIC dl pthread)
        else()
            message(STATUS "Warning: sqlite3 not found in libs/ or src/. Building without sqlite.")
            add_library(sqlite3 INTERFACE IMPORTED)
        endif()
    endif()
endif()

# ============================================================
# 3. nlohmann/json
# ============================================================
if(NOT TARGET nlohmann_json::nlohmann_json)
    add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED GLOBAL)
    set_property(TARGET nlohmann_json::nlohmann_json PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${TRIRD_PARTY_INCLUDE_DIR}")
endif()

# ============================================================
# 4. cpp-httplib (header-only HTTP server)
# ============================================================
if(EXISTS "${TRIRD_PARTY_INCLUDE_DIR}/httplib.h")
    message(STATUS "Using cpp-httplib from third_party/")
    add_library(httplib INTERFACE IMPORTED GLOBAL)
    set_property(TARGET httplib PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${TRIRD_PARTY_INCLUDE_DIR}")
endif()

# ============================================================
# 5. Standalone agent memory runtime (optional)
# ============================================================
option(ENABLE_MEMORY_BUILTIN "Build builtin memory runtime (requires agent-memory-cpp)" ON)

set(AGENT_MEMORY_CPP_DIR "${TRIRD_PARTY_SRC_DIR}/agent-memory-cpp-src" CACHE PATH "Path to agent-memory-cpp repository")
if(ENABLE_MEMORY_BUILTIN AND EXISTS "${AGENT_MEMORY_CPP_DIR}/CMakeLists.txt")
    set(AGENT_MEMORY_BUILD_TOOLS OFF CACHE BOOL "Build agent-memory-cpp tools" FORCE)
    set(AGENT_MEMORY_BUILD_TESTS OFF CACHE BOOL "Build agent-memory-cpp tests" FORCE)
    add_subdirectory("${AGENT_MEMORY_CPP_DIR}" "${CMAKE_BINARY_DIR}/agent-memory-cpp")
    message(STATUS "Builtin memory runtime enabled (agent-memory-cpp)")
elseif(ENABLE_MEMORY_BUILTIN)
    message(FATAL_ERROR "ENABLE_MEMORY_BUILTIN=ON but agent-memory-cpp not found: ${AGENT_MEMORY_CPP_DIR}")
else()
    message(STATUS "Builtin memory runtime disabled (ENABLE_MEMORY_BUILTIN=OFF)")
endif()

# ============================================================
# Core Library: agent_framework (pure core, no transport-layer deps)
# ============================================================
add_library(agent_framework SHARED

    src/core/agent.cpp
    src/core/agent_worker.cpp
    src/core/ask_user_dispatcher.cpp
    src/core/session_todo_list.cpp
    src/core/history_store.cpp
    src/core/dream_processor.cpp
    src/skills/skill_engine.cpp
    src/context_engine/context_engine.cpp
    src/context_engine/storage_base.cpp
    src/context_engine/json_storage.cpp
    src/context_engine/db_storage.cpp
    src/memory/http_memory_runtime.cpp
    src/memory/long_term_consolidator.cpp
    src/tools/builtin_tools/memory_read_payload_tool.cpp
    src/workers/agent_worker_factory.cpp
    src/workers/react_worker.cpp
    src/resource_manager/resource_manager.cpp
    src/tools/tool.cpp
    src/tools/tool_selector.cpp
    src/mcp/mcp_client.cpp
    src/mcp/mcp_connection.cpp
    src/mcp/mcp_tool.cpp
    src/mcp/mcp_config_manager.cpp
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
    src/tools/builtin_tools/skill_search_tool.cpp
    src/tools/builtin_tools/file_state_tool.cpp
    src/tools/builtin_tools/todo_tool.cpp
    src/tools/builtin_tools/ask_user_tool.cpp
    src/models/model.cpp
    src/models/openai_model.cpp
    src/models/anthropic_model.cpp
    src/utils/logger.cpp
    src/utils/data_dir.cpp
    src/utils/prompt_utils.cpp
    src/utils/encoding.cpp
    src/utils/tool_parser.cpp
    src/session/session_manager.cpp
    src/config/agent_config_json.cpp
    src/config/agent_config_store.cpp
    src/config/config_watcher.cpp
)

target_include_directories(agent_framework PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR} 
    ${CMAKE_CURRENT_SOURCE_DIR}/include 
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${TRIRD_PARTY_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
)

target_compile_definitions(agent_framework PRIVATE BUILDING_AGENT_FRAMEWORK)

# Builtin memory runtime is the only compilation unit that depends on
# agent-memory-cpp. When disabled, the core library builds without it and the
# http.server runtime remains available.
if(ENABLE_MEMORY_BUILTIN)
    target_sources(agent_framework PRIVATE
        src/memory/builtin_memory_runtime.cpp
        src/memory/type_bridge.cpp
    )
    target_include_directories(agent_framework PRIVATE ${AGENT_MEMORY_CPP_DIR}/include)
    target_link_libraries(agent_framework PRIVATE agent_memory)
    target_compile_definitions(agent_framework PRIVATE JIUWEN_ENABLE_MEMORY_BUILTIN)
endif()

# Core library only needs CURL/SQLite/JSON. No httplib, no OpenSSL.
if(WIN32)
    target_link_libraries(agent_framework PRIVATE 
        ${CURL_LIB}
        ${SQLITE_LIB}
        nlohmann_json::nlohmann_json
        ws2_32
        crypt32
    )
else()
    target_link_libraries(agent_framework PRIVATE 
        ${CURL_LIB}
        ${SQLITE_LIB}
        nlohmann_json::nlohmann_json
        pthread
        ${CMAKE_DL_LIBS}
    )
endif()

# ============================================================
# Example Application: jiuwenClaw
# Demonstrates how to integrate agent_framework with multiple
# access modes (CLI / HTTP / Feishu) from a single entry point.
# Adapters here are example-specific; downstream users may write
# their own adapters or use these as references.
# ============================================================

# --- Adapter: HTTP Server (REST API + SSE) ---
add_library(jiuwenClaw_http_server_adapter STATIC
    examples/jiuwenClaw/adapters/http_server/http_server.cpp
    examples/jiuwenClaw/channels/channel_manager.cpp
    examples/jiuwenClaw/channels/channel_service.cpp
    examples/jiuwenClaw/mcp/mcp_server_manager.cpp
)
target_include_directories(jiuwenClaw_http_server_adapter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(jiuwenClaw_http_server_adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${TRIRD_PARTY_INCLUDE_DIR}
)
target_link_libraries(jiuwenClaw_http_server_adapter PUBLIC agent_framework)
target_link_libraries(jiuwenClaw_http_server_adapter PRIVATE
    httplib
    nlohmann_json::nlohmann_json
    jiuwenClaw_feishu_adapter
)
if(WIN32)
    target_link_libraries(jiuwenClaw_http_server_adapter PRIVATE ws2_32 crypt32)
endif()
if(OpenSSL_FOUND)
    target_link_libraries(jiuwenClaw_http_server_adapter PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# --- Adapter: Feishu Bot (WebSocket long connection) ---
add_library(jiuwenClaw_feishu_adapter STATIC
    examples/jiuwenClaw/adapters/feishu/feishu_channel.cpp
    examples/jiuwenClaw/adapters/feishu/feishu_bot.cpp
)
target_include_directories(jiuwenClaw_feishu_adapter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(jiuwenClaw_feishu_adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${TRIRD_PARTY_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
)
target_link_libraries(jiuwenClaw_feishu_adapter PUBLIC agent_framework)
target_link_libraries(jiuwenClaw_feishu_adapter PRIVATE
    ${CURL_LIB}
    httplib
    nlohmann_json::nlohmann_json
)
if(WIN32)
    target_link_libraries(jiuwenClaw_feishu_adapter PRIVATE ws2_32 crypt32)
endif()
if(OpenSSL_FOUND)
    target_link_libraries(jiuwenClaw_feishu_adapter PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# --- Single-entry executable: jiuwenClaw (CLI + HTTP + Feishu) ---
add_executable(jiuwenClaw
    examples/jiuwenClaw/main.cpp
    examples/jiuwenClaw/heartbeat_manager.cpp
    examples/jiuwenClaw/cron_watcher.cpp
    examples/jiuwenClaw/tools/notebook_edit_tool.cpp
    examples/jiuwenClaw/tools/cron_tool.cpp
    examples/jiuwenClaw/tools/notify_tool.cpp
    examples/jiuwenClaw/utils/agent_response_handler.cpp
    examples/jiuwenClaw/utils/data_dir.cpp
    examples/jiuwenClaw/utils/encoding.cpp
    examples/jiuwenClaw/utils/logger.cpp
    examples/jiuwenClaw/utils/string_utils.cpp
)
target_include_directories(jiuwenClaw PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${TRIRD_PARTY_INCLUDE_DIR}
)
target_link_libraries(jiuwenClaw PRIVATE
    agent_framework
    jiuwenClaw_http_server_adapter
    jiuwenClaw_feishu_adapter
    nlohmann_json::nlohmann_json
)

# --- jiuwenClaw unit tests ---
add_executable(jiuwenClaw_tests
    unittest/main.cpp
    examples/jiuwenClaw/tests/test_notebook_edit_tool.cpp
    examples/jiuwenClaw/tools/notebook_edit_tool.cpp
)
target_include_directories(jiuwenClaw_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/unittest
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/jiuwenClaw/tools
    ${TRIRD_PARTY_INCLUDE_DIR}
)
target_link_libraries(jiuwenClaw_tests PRIVATE
    agent_framework
    nlohmann_json::nlohmann_json
)

# ============================================================
# Test Targets
# ============================================================
add_executable(unittest 
    unittest/main.cpp
    unittest/test_types.cpp
    unittest/test_tool.cpp
    unittest/test_context_engine.cpp
    unittest/test_agent_config.cpp
    unittest/test_memory_runtime.cpp
    unittest/test_resource_manager.cpp
    unittest/test_agent_factory.cpp
    unittest/test_tool_selector.cpp
    unittest/test_file_tools.cpp
    unittest/test_search_tools.cpp
    unittest/test_misc_tools.cpp
    unittest/test_integration.cpp
    unittest/test_openai_format.cpp
    unittest/test_anthropic_format.cpp
    unittest/test_session_tools.cpp
    unittest/test_retry_helper.cpp
)
target_include_directories(unittest PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR} 
    ${CMAKE_CURRENT_SOURCE_DIR}/include 
    ${CMAKE_CURRENT_SOURCE_DIR}/src 
    ${CMAKE_CURRENT_SOURCE_DIR}/unittest
)
target_link_libraries(unittest PRIVATE 
    agent_framework
    nlohmann_json::nlohmann_json
)
if(ENABLE_MEMORY_BUILTIN)
    target_include_directories(unittest PRIVATE ${AGENT_MEMORY_CPP_DIR}/include)
    target_compile_definitions(unittest PRIVATE JIUWEN_ENABLE_MEMORY_BUILTIN)
endif()

install(TARGETS agent_framework jiuwenClaw
    ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY examples/jiuwenClaw/ DESTINATION share/jiuwen-lite/examples)

# Copy web frontend to build directory for development
add_custom_command(TARGET jiuwenClaw POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/jiuwenClaw/web
        $<TARGET_FILE_DIR:jiuwenClaw>/web
    COMMENT "Copying web frontend to build directory"
)

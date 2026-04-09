cmake_minimum_required(VERSION 3.15)
project(jiuwen-lite LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Target definition
add_executable(jiuwen-lite
    src/core/agent.cpp
    src/core/agent_worker.cpp
    src/context_engine/context_engine.cpp
    src/context_engine/md_storage.cpp
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
    src/main.cpp
)

# Include directories (Scoped)
target_include_directories(jiuwen-lite PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Dependencies
# 1. nlohmann/json (FetchContent)
include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_GetProperties(json)
if(NOT json_POPULATED)
    FetchContent_Populate(json)
    add_subdirectory(${json_SOURCE_DIR} ${json_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

# 2. curl (FetchContent or System)
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

# Link libraries
target_link_libraries(jiuwen-lite PRIVATE 
    ${CURL_LIB}
    nlohmann_json::nlohmann_json
)

# Install rules for future integration (Agent Framework)
install(TARGETS jiuwen-lite
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)

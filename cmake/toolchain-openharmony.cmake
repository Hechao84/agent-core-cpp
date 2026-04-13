# Toolchain file for OpenHarmony (API 9+)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm64) # or x86_64

# OpenHarmony uses Clang from the NDK
# You must set OHOS_NDK_HOME environment variable or replace this path
set(OHOS_NDK "/path/to/openharmony/ndk/native" CACHE PATH "OHOS NDK path")
set(CMAKE_C_COMPILER   ${OHOS_NDK}/llvm/bin/clang)
set(CMAKE_CXX_COMPILER ${OHOS_NDK}/llvm/bin/clang++)

# Compiler flags for OpenHarmony
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --target=arm64-linux-ohos --sysroot=${CMAKE_SYSROOT}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --target=arm64-linux-ohos --sysroot=${CMAKE_SYSROOT}" CACHE STRING "" FORCE)

# OpenHarmony uses libc++
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

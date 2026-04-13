# Toolchain file for Linux (Native or Cross to ARM64)
set(CMAKE_SYSTEM_NAME Linux)

# For native compilation, these can point to system compilers
# For cross-compilation (e.g., aarch64), point to the cross-compiler:
# set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
# set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Set find root path for cross-compilation sysroot
# set(CMAKE_FIND_ROOT_PATH /path/to/sysroot)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

#!/bin/bash
# build_linux.sh - Script to build the Linux version of the Agent Framework

set -e

echo "=== Building for Linux ==="

# Create build directory
mkdir -p build-linux
cd build-linux

# Configure CMake
# This toolchain file is for native x64 Linux compilation.
# For ARM64/Android/OpenHarmony, change the toolchain file.
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-linux.cmake \
    -DCMAKE_CXX_FLAGS="-std=c++17"

# Build
cmake --build . --target jiuwen-lite -j$(nproc)

# Package
mkdir -p ../dist/linux
cp jiuwen-lite ../dist/linux/
ls -lh ../dist/linux/

echo "=== Build Complete ==="
echo "Deliverable located at: dist/linux/jiuwen-lite"

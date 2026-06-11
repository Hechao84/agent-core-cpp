#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build-memory-linux"
DIST_DIR="$SCRIPT_DIR/dist/memory-runtime/linux"

cd "$SCRIPT_DIR"

bash "$SCRIPT_DIR/build_third_party.sh"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-std=c++17"

NPROC=$(nproc)
cmake --build . --target agent_framework memory-server memory-mcp-server -- -j"$NPROC"

mkdir -p "$DIST_DIR/bin" "$DIST_DIR/lib" "$DIST_DIR/include" "$DIST_DIR/examples"
cp memory-server "$DIST_DIR/bin/"
cp memory-mcp-server "$DIST_DIR/bin/"
cp libagent_framework.so "$DIST_DIR/lib/"
cp -r "$SCRIPT_DIR/include"/* "$DIST_DIR/include/"
cp -r "$SCRIPT_DIR/examples/memory_server" "$DIST_DIR/examples/"

if [ -d "$SCRIPT_DIR/libs" ]; then
    cp -r "$SCRIPT_DIR/libs" "$DIST_DIR/"
fi

echo "Memory Runtime build complete: dist/memory-runtime/linux"

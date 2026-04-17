#!/bin/bash
# build_linux.sh - Build the entire project
# Step 1: Build third-party shared libraries (calls build_third_party.sh)
# Step 2: Build main project (links to pre-built .so files)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ============================================================
# Step 1: Build third-party libraries
# ============================================================
echo "=========================================="
echo " [1/3] Building Third-Party Libraries"
echo "=========================================="

bash "$SCRIPT_DIR/build_third_party.sh"

echo ""

# ============================================================
# Step 2: Configure Main Project
# ============================================================
echo "=========================================="
echo " [2/3] Configuring Main Project"
echo "=========================================="

BUILD_DIR="$SCRIPT_DIR/build-linux"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "--- Running CMake..."
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-std=c++17"

echo "--- CMake configuration complete."
echo ""

# ============================================================
# Step 3: Build Main Project
# ============================================================
echo "=========================================="
echo " [3/3] Building Main Project"
echo "=========================================="

NPROC=$(nproc)
echo "--- Compiling with $NPROC threads..."
echo "--- This may take a minute. Progress:"

cmake --build . --target jiuwenClaw -- -j"$NPROC"

# Package
echo ""
echo "--- Packaging output..."
mkdir -p "$SCRIPT_DIR/dist/linux"
cp jiuwenClaw "$SCRIPT_DIR/dist/linux/"
cp libagent_framework.so "$SCRIPT_DIR/dist/linux/"

# Copy skills directory if they exist
if [ -d "$SCRIPT_DIR/my_skills" ]; then
    cp -r "$SCRIPT_DIR/my_skills" "$SCRIPT_DIR/dist/linux/"
    echo "Skills copied to dist/linux/my_skills"
fi

echo ""
echo "=========================================="
echo " Build Complete!"
echo "=========================================="
echo "Library: dist/linux/libagent_framework.so"
ls -lh "$SCRIPT_DIR/dist/linux/libagent_framework.so"
echo "Binary:  dist/linux/jiuwenClaw"
ls -lh "$SCRIPT_DIR/dist/linux/jiuwenClaw"
echo ""
echo "To run the demo app:"
echo "  LD_LIBRARY_PATH=$SCRIPT_DIR/libs:$SCRIPT_DIR/dist/linux ./dist/linux/jiuwenClaw"

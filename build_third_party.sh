#!/bin/bash
# build_third_party.sh - Download and compile third-party base dependencies
# Output: third_party/include/*.hpp and libs/*.so
# This script is meant to be called by build_linux.sh

set -e

# Number of parallel jobs
NPROC=$(nproc)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Directories
LIBS_DIR="$SCRIPT_DIR/libs"
TP_DIR="$SCRIPT_DIR/third_party"
INCLUDE_DIR="$TP_DIR/include"
SRC_DIR="$TP_DIR/src"
BUILD_DIR="$TP_DIR/build"

mkdir -p "$LIBS_DIR" "$INCLUDE_DIR" "$SRC_DIR" "$BUILD_DIR"

# ============================================================
# Helper: Clone repository
# ============================================================
clone() {
    local repo="$1" tag="$2" dest="$3"
    local mirror="${repo/github.com\/mirror.ghproxy.com\/https:\/\/github.com\/}"

    [ -d "$dest/.git" ] || [ -f "$dest/CMakeLists.txt" ] && return 0

    echo "  Cloning $(basename $repo) $tag..."
    git clone --single-branch --branch "$tag" --depth 1 "$mirror" "$dest" 2>/dev/null || \
    git clone --single-branch --branch "$tag" --depth 1 "$repo" "$dest" 2>/dev/null || \
    { echo "  FAILED: $repo"; return 1; }
}

echo "=== Building Third-Party Base Libraries ==="

# ============================================================
# 1. nlohmann/json (header-only)
# ============================================================
if [ ! -f "$INCLUDE_DIR/nlohmann/json.hpp" ]; then
    echo "[1/2] Building nlohmann/json..."
    clone "https://github.com/nlohmann/json.git" "v3.11.3" "$SRC_DIR/nlohmann_json-src"
    mkdir -p "$INCLUDE_DIR/nlohmann"
    cp -f "$SRC_DIR/nlohmann_json-src/single_include/nlohmann/json.hpp" "$INCLUDE_DIR/nlohmann/"
    echo "  nlohmann/json ready."
else
    echo "[1/2] nlohmann/json already built, ready."
fi

# ============================================================
# 2. sqlite3 (shared)
# ============================================================
if [ ! -f "$LIBS_DIR/libsqlite3.so" ]; then
    echo "[2/2] Building sqlite3..."
    local_zip="$BUILD_DIR/sqlite3.zip"
    
    # Download
    if [ ! -f "$SRC_DIR/sqlite3-src/sqlite3.c" ]; then
        if [ ! -f "$local_zip" ]; then
            echo "  Downloading from sqlite.org..."
            if command -v wget >/dev/null 2>&1; then
                wget --no-check-certificate "https://www.sqlite.org/2024/sqlite-amalgamation-3450300.zip" -O "$local_zip" || \
                    { echo "  wget failed, trying curl..."; curl -kL "https://www.sqlite.org/2024/sqlite-amalgamation-3450300.zip" -o "$local_zip"; }
            else
                curl -kL "https://www.sqlite.org/2024/sqlite-amalgamation-3450300.zip" -o "$local_zip"
            fi
        else
            echo "  Using cached download."
        fi
        
        if [ -f "$local_zip" ] && [ -s "$local_zip" ]; then
            echo "  Extracting sqlite3..."
            mkdir -p "$SRC_DIR/sqlite3-src"
            rm -rf "$BUILD_DIR"/sqlite-amalgamation-*
            unzip -o "$local_zip" -d "$BUILD_DIR/" 2>/dev/null || python3 -c "import zipfile,os; zipfile.ZipFile('$local_zip','r').extractall('$BUILD_DIR/')"
            
            src_dir=$(ls -d "$BUILD_DIR"/sqlite-amalgamation-* 2>/dev/null | head -1)
            [ -d "$src_dir" ] && cp -f "$src_dir/sqlite3.c" "$src_dir/sqlite3.h" "$src_dir/sqlite3ext.h" "$SRC_DIR/sqlite3-src/"
        fi
    fi
    
    # Compile as shared library
    if [ -f "$SRC_DIR/sqlite3-src/sqlite3.c" ]; then
        echo "  Compiling libsqlite3.so with $NPROC threads..."
        gcc -shared -fPIC -O2 -o "$LIBS_DIR/libsqlite3.so" \
            "$SRC_DIR/sqlite3-src/sqlite3.c" -I"$SRC_DIR/sqlite3-src" -ldl -lpthread
        cp -f "$SRC_DIR/sqlite3-src/sqlite3.h" "$INCLUDE_DIR/"
        echo "  sqlite3 ready."
    else
        echo "  WARNING: sqlite3 source not found."
    fi
else
    echo "[2/2] sqlite3 already built, ready."
fi

# ============================================================
# Summary
# ============================================================
echo ""
echo "=== Third-Party Base Libraries Ready ==="
ls -lh "$LIBS_DIR/"*.so* 2>/dev/null || echo "  (no .so files)"
echo "$(find "$INCLUDE_DIR" -name '*.h' -o -name '*.hpp' 2>/dev/null | wc -l) headers installed to third_party/include/"
echo "=== Done ==="

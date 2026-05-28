# Third-party Dependencies

> English | [中文](#中文)

This directory stores third-party library source code and build artifacts for this project.

## Build Mechanism

This project uses a **mixed mechanism of precompiled shared libraries + source compilation**:

1. **Base Libraries (`build_third_party.sh` / `build_third_party.bat`)**:
   - Responsible for downloading and compiling base dependencies (such as `sqlite3`, `curl`, `libevent`, etc.).
   - The compiled `libxxx.so` (or `.dll`) will be stored in the project root's `libs/` directory.
   - Corresponding header files will be installed under `third_party/include/`.
   - **Note**: Do not manually commit compiled binary files to the code repository.

2. **Main Project Compilation (`build_linux.sh` / `build_windows.bat`)**:
   - Automatically calls `build_third_party` script before compiling the main project to ensure dependencies are ready.
   - Links precompiled libraries from `libs/` via CMake.

## How to Add Dependencies

1. Add the corresponding download and compilation logic in `build_third_party.sh` (and `build_third_party.bat` for Windows).
2. Ensure compilation outputs are placed in the `libs/` directory.
3. Ensure header files are copied to `third_party/include/`.
4. Update `CMakeLists.txt` to link the new library.

## Directory Structure

```text
third_party/
├── include/          <-- Installed third-party library headers
├── libs/             <-- Precompiled third-party shared libraries
├── src/              <-- Downloaded source code and intermediate build directories
├── build/            <-- Build helper scripts
├── feishu/           <-- Feishu/Lark integration helpers
└── README.md
```

---

# 中文

> [English](#third-party-dependencies) | 中文

此目录用于存放本工程依赖的三方库源码及构建产物。

## 构建机制

本工程采用**预编译共享库 + 源码编译**混合机制：

1. **基础三库 (`build_third_party.sh` / `build_third_party.bat`)**:
   - 负责下载并编译基础依赖（如 `sqlite3`、`curl`、`libevent` 等）。
   - 编译生成的 `libxxx.so`（或 `.dll`）将存放于项目根目录的 `libs/` 中。
   - 对应的头文件将安装到 `third_party/include/` 下。
   - **注意**：不要手动提交编译出的二进制文件到代码仓库。

2. **主工程编译 (`build_linux.sh` / `build_windows.bat`)**:
   - 在编译主工程前，自动调用 `build_third_party` 脚本确保依赖就绪。
   - 通过 CMake 链接 `libs/` 中的预编译库。

## 如何添加依赖

1. 在 `build_third_party.sh`（及 Windows 对应的 `build_third_party.bat`）中添加对应的下载与编译逻辑。
2. 确保编译输出放置到 `libs/` 目录。
3. 确保头文件被拷贝到 `third_party/include/`。
4. 更新 `CMakeLists.txt` 以链接新的库。

## 目录结构

```text
third_party/
├── include/          <-- 安装后的三方库头文件
├── libs/             <-- 预编译的三方库共享库
├── src/              <-- 下载的源码及中间构建目录
├── build/            <-- 构建辅助脚本
├── feishu/           <-- 飞书集成辅助工具
└── README.md
```

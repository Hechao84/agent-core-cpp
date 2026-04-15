# 3rd-party Dependencies

此目录用于存放本工程依赖的三方库源码及构建产物。

## 构建机制

本工程采用**预编译共享库 + 源码编译**混合机制：

1. **基础三库 (`build_third_party.sh`)**:
   - 负责下载并编译基础依赖（如 `libevent`, `sqlite3` 等）。
   - 编译生成的 `libxxx.so` 将存放于项目根目录的 `libs/` 中。
   - 对应的头文件将安装到 `src/3rd-party/include/` 下。
   - **注意**：不要手动提交编译出的二进制文件到代码仓库。

2. **主工程编译 (`build_linux.sh`)**:
   - 在编译主工程前，自动调用 `build_third_party.sh` 确保依赖就绪。
   - 通过 CMake 链接 `libs/` 中的预编译库。

## 如何添加依赖

1. 在 `build_third_party.sh` 中添加对应的下载与编译逻辑。
2. 确保编译输出放置到 `libs/` 目录。
3. 确保头文件被拷贝到 `src/3rd-party/include/`。
4. 更新 `CMakeLists.txt` 以链接新的库。

## 目录结构

```text
src/3rd-party/
├── include/          <-- 安装后的三方库头文件
├── src/              <-- 下载的源码及中间构建目录
└── README.md
```

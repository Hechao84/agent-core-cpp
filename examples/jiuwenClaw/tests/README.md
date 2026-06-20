# jiuwenClaw Tests

jiuwenClaw 应用层专属测试，测试 jiuwenClaw 独有的工具和功能（不包含在核心库 `agent_framework` 中）。

## 编译运行

使用顶层 CMake 构建：

```bash
cmake --build build-linux --target jiuwenClaw_tests -- -j$(nproc)
./build-linux/jiuwenClaw_tests
```

运行时会自动 chdir 到项目根目录，测试产生的临时目录（`test_tmp_*`）会在结束后自动清理。

## 测试列表

| 测试文件 | 覆盖范围 |
|---|---|
| `test_notebook_edit_tool.cpp` | NotebookEditTool 单元测试（6 个用例） |

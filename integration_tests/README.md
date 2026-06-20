# Integration Tests

本目录包含需要外部依赖（真实 API Key、网络访问等）的集成测试和端到端测试。

这些测试**不纳入 CMake 默认构建**，也不在 CI 中运行。

## 测试列表

| 文件 | 说明 | 依赖 |
|---|---|---|
| `e2e_agent_react.cpp` | 完整 Agent ReAct 循环端到端测试，跑 3 个真实查询 | 模型 API Key + 网络 |
| `smoke_web_search.cpp` | web_search 工具真实网络调用冒烟 | 网络（HTTPS） |

## 编译方式

手动编译，链接 `agent_framework` 共享库。示例（Linux）：

```bash
g++ -std=c++17 -I. -Iinclude -Isrc -Ithird_party/include \
    integration_tests/e2e_agent_react.cpp \
    -Lbuild-linux -lagent_framework -lpthread -ldl \
    -o build-linux/e2e_agent_react
```

运行前需设置 `LD_LIBRARY_PATH=./build-linux:./libs`。

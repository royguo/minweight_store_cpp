# minweight_store_cpp

`minweight_store_cpp` 是 `minweight_store` 的 C++17/20 结构化移植仓库。

当前 `main` 分支已经重建为 C++ 版本的初始骨架。原 Go 实现保存在：

- 远端分支：`golang`
- 本地临时快照：`golang_code/`，该目录已被 `.gitignore` 忽略，仅用于重构期间快速 review 旧代码

## 目标

本仓库的目标不是逐行翻译 Go 代码，而是保留核心算法、磁盘状态机和崩溃恢复语义，同时把运行时边界设计为可插拔形式，使核心引擎可以适配标准线程、bthread/butex 或其他 coroutine/fiber runtime。

首个里程碑是完成仓库内置的 `minpatricia` C++ 实现，以及基于它的 heap-backed in-memory KV store。持久化 WAL、manifest、mmap node store、SST 和 compaction 在后续里程碑中推进。

## 目录

```text
.
|-- CMakeLists.txt
|-- docs/
|   |-- agents_read.md
|   |-- design/
|   |-- discuss/
|   |-- trackers/
|   `-- worklogs/
|-- src/
|   |-- minpatricia/
|   `-- minweight_store/
`-- third/
```

## 构建

当前阶段只有 CMake 骨架和接口目标，后续源码会逐步补齐。

```bash
cmake -S . -B build -DMINWEIGHT_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

## 开发规则

开始任务前先阅读 `docs/agents_read.md`，再同步 `docs/trackers/todos.md`、`docs/design/index.md` 和相关文档。新测试与被测试源码放在同一模块目录内，例如 `src/minpatricia/patricia_trie_test.cc`。

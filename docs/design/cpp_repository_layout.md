# C++ 仓库目录设计

## 目标

本仓库采用 CMake 构建，第三方依赖集中在 `third/`，核心源码集中在 `src/`。测试代码与被测试源码放在同一模块目录，避免额外顶层 `test/` 目录。

`minpatricia` 是 store 行为的一部分，不作为外部依赖处理，统一合并到当前仓库内实现。

## 顶层目录

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
|   |-- runtime/
|   `-- storage/
`-- third/
```

## `src/minpatricia`

`minpatricia` 是纯 ordered index library。

职责：

- Patricia trie 的 node page、rep layout、insert、delete、seek、scan、retarget。
- `Position` opaque handle 与 child-node tag 语义。
- `NodeStore` 和 `RecordStore` 抽象。
- golden tests，用于对齐 Go 版本行为。

不负责：

- 文件 I/O。
- mmap 生命周期。
- WAL、manifest、SST。
- coroutine 或后台调度。

建议目标：

```text
minweight_minpatricia
```

## `src/runtime`

`runtime` 定义可插拔运行时边界。

职责：

- `Env`/`Runtime` 抽象。
- `StdEnv` 实现。
- 后续 `BthreadEnv` 或其他 runtime adapter。

核心原则是 store core 不直接返回某个具体 coroutine library 的 `Task<T>`，也不直接依赖具体 mutex 或 background task 类型。

建议目标：

```text
minweight_runtime
```

## `src/storage`

`storage` 实现 KV engine。

职责：

- public store API。
- WAL、manifest、mmap node store。
- primary index 与 secondary checkpoint index。
- flush、recovery、minor compaction、major compaction。
- SST backend 抽象与具体实现。

读取路径必须始终使用 primary index。checkpoint index 只服务 flush 与 recovery。

建议目标：

```text
minweight_storage
```

## 测试布局

测试文件与被测源码放在同目录。

示例：

```text
src/minpatricia/
|-- node_page.cc
|-- node_page.h
|-- node_page_test.cc
|-- patricia_trie.cc
|-- patricia_trie.h
`-- patricia_trie_test.cc

src/storage/
|-- wal_record.cc
|-- wal_record.h
|-- wal_record_test.cc
|-- manifest.cc
|-- manifest.h
`-- manifest_test.cc
```

## `third`

第三方依赖统一放在 `third/`。新增依赖前需要明确：

- 是否必须 vendored。
- 是否影响二进制体积和构建时间。
- 是否引入异常、RTTI、线程模型或 allocator 约束。
- 是否能被 `StdEnv` 和未来 fiber runtime 共同使用。

SST 是否继续使用 Parquet 兼容格式仍未定案，决策前先在 `discuss/` 中记录取舍。

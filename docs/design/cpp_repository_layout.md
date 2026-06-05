# C++ 仓库目录设计

## 目标

本仓库采用 CMake 构建，第三方依赖集中在 `third/`，核心源码集中在 `src/`。测试代码与被测试源码放在同一模块目录，避免额外顶层 `test/` 目录。

最终对外编译产物只保留两个：

```text
产物             类型      用途
---------------  --------  ----------------------------------------------
minpatricia      library   独立 ordered index，可被外部项目单独使用
minweight_store  library   KV store 主库，依赖并链接 minpatricia
```

`minpatricia` 是 store 行为的一部分，但它也需要能被外部项目单独使用。因此它不放入 `third/`，而是在当前仓库中作为一等源码模块实现，并生成独立 CMake target。

`runtime` 不作为第三个最终产物。标准线程、bthread、I/O offload、锁与后台任务等运行时适配能力应收敛在 `minweight_store` 模块内，通过 public 或 internal header 形成边界，最终编译进 `minweight_store`。

## 推荐顶层目录

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

## `src/minpatricia`

`src/minpatricia` 生成独立产物 `minpatricia`。

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

推荐目录：

```text
src/minpatricia/
|-- CMakeLists.txt
|-- include/
|   `-- minpatricia/
|       |-- byte_view.h
|       |-- index.h
|       |-- node_page.h
|       |-- node_store.h
|       |-- position.h
|       |-- record_store.h
|       `-- status.h
|-- index.cc
|-- index_test.cc
|-- node_page.cc
|-- node_page_test.cc
|-- seek_test.cc
`-- golden_test.cc
```

公共头文件放在 `include/minpatricia/`，外部用户应能通过如下方式使用：

```cpp
#include <minpatricia/index.h>
```

CMake target：

```cmake
add_library(minpatricia ...)
add_library(minpatricia::minpatricia ALIAS minpatricia)
```

`minpatricia` 的 public API 应包含外部实现 `NodeStore`/`RecordStore` 所需的最小类型，例如 `Position`、`ByteView`、`NodePage`、`NodeStore`、`RecordStore` 和 `Status`/`Result`。不应暴露 WAL、mmap、manifest 或 store 内部概念。

## `src/minweight_store`

`src/minweight_store` 生成主产物 `minweight_store`，并链接 `minpatricia`。

职责：

- public store API：`Put`、`Get`、`Delete`、`Len`、`SeekGE`、`SeekLE`、`Scan`、`ScanRange`、`ReverseScan`、`ReverseScanRange`。
- heap-backed in-memory store。
- WAL、manifest、mmap node store。
- primary index 与 secondary checkpoint index。
- flush、recovery、minor compaction、major compaction。
- SST backend 抽象与具体实现。
- `Env`/`Runtime` 抽象、`StdEnv`、后续 `BthreadEnv` 或 I/O offload adapter。

读取路径必须始终使用 primary index。checkpoint index 只服务 flush 与 recovery。

推荐目录：

```text
src/minweight_store/
|-- CMakeLists.txt
|-- include/
|   `-- minweight_store/
|       |-- env.h
|       |-- options.h
|       |-- status.h
|       |-- store.h
|       `-- types.h
|-- internal/
|   |-- compaction/
|   |-- manifest/
|   |-- mmap/
|   |-- record/
|   |-- runtime/
|   |-- sst/
|   `-- wal/
|-- store.cc
|-- store_test.cc
|-- open.cc
|-- open_test.cc
|-- in_memory_store.cc
`-- in_memory_store_test.cc
```

公共头文件放在 `include/minweight_store/`，外部用户应能通过如下方式使用：

```cpp
#include <minweight_store/store.h>
```

CMake target：

```cmake
add_library(minweight_store ...)
add_library(minweight_store::minweight_store ALIAS minweight_store)
target_link_libraries(minweight_store PUBLIC minpatricia)
```

## Runtime 放置规则

`Env` 是 `minweight_store` 的运行时边界，不是独立库产物。这样做的原因是：

```text
目标                                处理方式
----------------------------------  ------------------------------------------
只单独使用 minpatricia              不引入任何 runtime、文件 I/O 或后台任务
使用 minweight_store + StdEnv        链接 minweight_store 即可
使用 minweight_store + BthreadEnv    仍链接 minweight_store，按配置选择 runtime adapter
```

如果未来需要把 runtime adapter 拆得更细，可以在 CMake 内部使用 object library 或 private source group，但 install/export 的最终产物仍应只暴露 `minpatricia` 和 `minweight_store`。

`Env` 最少需要覆盖：

- mutex、rw mutex、condition/event、semaphore。
- background task spawn、join、cancel/stop。
- time source、logging。
- mmap、munmap、msync。
- open/read/write/rename/remove/fsync/directory sync。
- 可选 I/O offload 或 fiber-aware blocking primitive。

## CMake 组织

顶层 `CMakeLists.txt` 只负责全局选项和子目录：

```cmake
add_subdirectory(src/minpatricia)
add_subdirectory(src/minweight_store)
```

`src/minweight_store/CMakeLists.txt` 直接收集主库源码，并 `PUBLIC` 链接 `minpatricia`。不要把 runtime、wal、manifest、sst 单独做成对外库 target。

测试在 `MINWEIGHT_BUILD_TESTS=ON` 时生成测试可执行文件。测试可执行文件不是 install/export 产物。

## 测试布局

测试文件与被测源码放在同目录或同模块子目录。

示例：

```text
src/minpatricia/
|-- node_page.cc
|-- node_page.h
|-- node_page_test.cc
|-- patricia_trie.cc
|-- patricia_trie.h
`-- patricia_trie_test.cc

src/minweight_store/internal/wal/
|-- wal_record.cc
|-- wal_record.h
`-- wal_record_test.cc

src/minweight_store/internal/manifest/
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

## 迁移当前骨架

当前初始骨架中已有 `src/storage/` 和 `src/runtime/`。进入实现前应调整为：

```text
当前路径        目标路径
--------------  ------------------------------------------------
src/storage/    src/minweight_store/ 及其 internal 子目录
src/runtime/    src/minweight_store/internal/runtime/ 或 public env header
```

调整后，仓库的源码模块与最终产物名称保持一致，降低外部用户和 CMake install/export 的理解成本。

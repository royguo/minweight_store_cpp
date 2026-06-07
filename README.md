# minweight_store_cpp

`minweight_store_cpp` 来源于原 Go/golang 版本的 `minweight_store`。本仓库不是在原 Go 代码上继续维护，而是使用 C++17 重新构建同一套核心能力：保留原实现中的 ordered index、record position、WAL、manifest、checkpoint、SST install、compaction 和 crash recovery 语义，同时重新设计 C++ 下的模块边界、运行时边界和构建方式。

原 Go 实现保存在：

- 远端分支：`golang`
- 本地临时快照：`golang_code/`，该目录被 `.gitignore` 忽略，仅用于重构期间 review 旧代码

当前仓库采用 CMake 构建，最终对外保留两个 library：

```text
产物             状态      用途
---------------  --------  ------------------------------------------------------------
minpatricia      Active    独立 ordered index，可被外部项目单独使用
minweight_store  WIP       KV store 主库，WAL-backed 主链路已实现，SST/compaction 尚未完成
```

## minpatricia

`minpatricia` 是本仓库已经完成的第一个核心模块，位于 `src/minpatricia/`。它是一个 C++17 ordered Patricia index，负责维护 byte string key 到 opaque `Position` 的映射。

核心特点：

- 可作为独立 C++ library 使用，不依赖 `minweight_store`。
- public API 覆盖 Go 版 `minpatricia` 的 `Get`、`Probe`、`Put`、`Delete`、`Retarget`、正向/反向遍历和 range seek。
- key 使用仓库内置 `minpatricia::Span<const std::byte>` 表达零拷贝 `ByteView`，不依赖 C++20 `std::span`。
- 默认热路径使用 templates 静态分发，并通过 C++17 traits 做 store 约束，不通过 virtual interface。
- `NodePage` 固定 4096 字节，最多容纳 339 个 rep，便于后续接入 mmap-backed node store。
- `RecordStore` 和 `NodeStore` 解耦，调用方可以使用内置 heap store，也可以接入自定义存储后端。
- 不负责 WAL、manifest、mmap 生命周期、SST、锁、协程 runtime 或 value 生命周期。

`minpatricia` 的详细设计文档：

```text
docs/design/minpatricia.md
docs/design/minpatricia_go_ut_coverage.md
docs/design/minpatricia_port_plan.md
```

### 性能结果

当前 C++17 版 benchmark 使用 Go-generated fixed fixtures，确保 1K/10K/100K keyset 与 Go benchmark 同源。2026-06-07 同机 Release 结果显示，C++ 版本在核心操作上保持接近或快于 Go 对照，node footprint 与 Go 对齐。

```text
指标             1K C++ / Go       10K C++ / Go      100K C++ / Go
---------------  ----------------  ----------------  ----------------
Get              62 / 78.23 ns     117 / 134.4 ns    160 / 184.7 ns
PutReplace       94 / 91.46 ns     143 / 146.5 ns    191 / 195.9 ns
PutInsert        423 / 401.9 ns    579 / 568.1 ns    691 / 697.3 ns
Seek             100 / 113.2 ns    153 / 162.2 ns    191 / 213.4 ns
ReverseSeek      109 / 115.0 ns    156 / 164.9 ns    192 / 215.4 ns
DeleteHeavy      n/a               122 / 126.8 ns    150 / 155.4 ns
Footprint nodes  4 / 4             50 / 50           515 / 515
```

遍历性能：

```text
指标                         1K C++ / Go         10K C++ / Go        100K C++ / Go
---------------------------  ------------------  ------------------  ------------------
VisitFullSetOrdered          4 / 11.105 ns/item  5 / 11.198 ns/item  7 / 16.570 ns/item
VisitFullSetReverse          4 / 10.484 ns/item  5 / 10.558 ns/item  8 / 16.049 ns/item
```

历史 Go/C++ benchmark 归档文件在本地 `.runtime/` 下，不进入 git：

```text
.runtime/benchmarks/2026-06-05/cpp_minpatricia_bench_large.txt
.runtime/benchmarks/2026-06-05/go_minpatricia_bench_large.txt
```

### 使用方式

使用 heap-backed store：

```cpp
#include <cassert>
#include <string>

#include "minpatricia/minpatricia.h"

void Example() {
  auto heap = minpatricia::NewHeap<std::string>();
  assert(heap.ok());

  auto tree = heap.take_value();
  const auto key = minpatricia::AsBytes("alice");
  const minpatricia::Position pos = tree.records->Add(key, "value-1");

  auto put = tree.index.Put(key, pos);
  assert(put.ok());

  auto found = tree.index.Get(key);
  assert(found.ok() && found.value().found);
}
```

外部项目也可以实现自己的 `RecordStore` 和 `NodeStore`，再通过：

```cpp
auto index = minpatricia::Index<MyRecordStore, MyNodeStore>::NewWithNodes(records, nodes);
```

## minweight_store

`minweight_store` 是 C++ 版 KV store 主库，位于 `src/minweight_store/`。它依赖并链接 `minpatricia`，用于承载原 Go 版 `minweight_store` 的完整存储引擎能力。

当前已经实现：

- public KV API：`Open`、`Close`、`Put`、`Get`、`Delete`、`Len`、`SeekGE`、`SeekLE`、`Scan`、`ScanRange`、`ReverseScan`、`ReverseScanRange`。
- WAL-backed record store，使用 mmap append，默认不在每次写入时主动 `msync`。
- WAL record CRC 校验、WAL rollover、point-in-time prefix replay 和 strict replay。
- primary index 使用 `minpatricia`，启动时从 WAL 重建。
- `Runtime` 抽象与 `StdRuntime`，主链路可以注入不同锁和 blocking I/O 实现，后续可接 bthread/butex runtime。

当前写入语义是高性能、弱持久化：`Put` / `Delete` 返回表示进程内可见，不代表每次写入已经落盘。crash recovery 默认只接受 WAL 中 CRC-valid 的连续前缀，允许丢失尾部写入，但不跳过中间坏记录恢复后续写入。

尚未完成：

- manifest。
- mmap node store 与 primary/secondary checkpoint。
- SST backend。
- minor compaction 和 major compaction。
- bthread/butex runtime adapter。

`minweight_store` 的详细设计文档：

```text
docs/design/minweight_store.md
```

### 当前性能结果

当前 benchmark 对比的是 WAL-backed 主链路，key/value 生成方式一致，value size 为 64B，Go 版关闭 minor/major compaction 后运行。2026-06-07 同机 Release 结果：

```text
指标        1K C++ / Go      10K C++ / Go     100K C++ / Go
----------  ---------------  ---------------  ---------------
Put         556 / 790 ns     602 / 748 ns     604 / 753 ns
Get         125 / 206 ns     150 / 233 ns     155 / 226 ns
Scan        81 / 182 ns      82 / 198 ns      77 / 174 ns
SeekGE      202 / 319 ns     211 / 316 ns     221 / 348 ns
```

这组数据不包含 Go 版 SST/compaction 与 C++ 版未来 manifest/checkpoint 的成本，主要用于验证当前主写读链路没有性能回退。

### 使用方式

```cpp
#include <cassert>

#include "minweight_store/store.h"

void Example() {
  minweight_store::Options options;
  auto opened = minweight_store::Store::Open("/path/to/store", options);
  assert(opened.ok());

  auto store = opened.take_value();
  assert(store->Put(minweight_store::AsBytes("alice"),
                    minweight_store::AsBytes("value-1")).ok());

  auto got = store->Get(minweight_store::AsBytes("alice"));
  assert(got.ok() && got.value().found);
}
```

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

```bash
cmake -S . -B build -DMINWEIGHT_BUILD_TESTS=ON -DMINWEIGHT_BUILD_BENCHMARKS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

运行 `minpatricia` benchmark：

```bash
./build/src/minpatricia/minpatricia_bench
MINPATRICIA_BENCH_LARGE=1 ./build/src/minpatricia/minpatricia_bench
```

运行 `minweight_store` benchmark：

```bash
./build/src/minweight_store/minweight_store_bench
MINWEIGHT_STORE_BENCH_LARGE=1 ./build/src/minweight_store/minweight_store_bench
```

## 开发规则

开始任务前先阅读 `docs/agents_read.md`，再同步 `docs/trackers/todos.md`、`docs/design/index.md` 和相关文档。新测试与被测试源码放在同一模块目录内。

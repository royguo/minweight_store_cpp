# minweight_store_cpp

`minweight_store_cpp` 来源于原 Go/golang 版本的 `minweight_store`。本仓库不是在原 Go 代码上继续维护，而是使用 C++20 重新构建同一套核心能力：保留原实现中的 ordered index、record position、WAL、manifest、checkpoint、SST install、compaction 和 crash recovery 语义，同时重新设计 C++ 下的模块边界、运行时边界和构建方式。

原 Go 实现保存在：

- 远端分支：`golang`
- 本地临时快照：`golang_code/`，该目录被 `.gitignore` 忽略，仅用于重构期间 review 旧代码

当前仓库采用 CMake 构建，最终对外保留两个 library：

```text
产物             状态      用途
---------------  --------  ------------------------------------------------------------
minpatricia      Active    独立 ordered index，可被外部项目单独使用
minweight_store  WIP       KV store 主库，依赖 minpatricia，目前尚未翻译完成
```

## minpatricia

`minpatricia` 是本仓库已经完成的第一个核心模块，位于 `src/minpatricia/`。它是一个 C++20 ordered Patricia index，负责维护 byte string key 到 opaque `Position` 的映射。

核心特点：

- 可作为独立 C++ library 使用，不依赖 `minweight_store`。
- public API 覆盖 Go 版 `minpatricia` 的 `Get`、`Probe`、`Put`、`Delete`、`Retarget`、正向/反向遍历和 range seek。
- key 使用 `std::span<const std::byte>` 表达零拷贝 `ByteView`。
- 默认热路径使用 C++20 concepts/templates 静态分发，不通过 virtual interface。
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

当前 C++ 版 benchmark 使用 Go-generated fixed fixtures，确保 1K/10K/100K keyset 与 Go benchmark 同源。2026-06-05 同机结果显示，C++ 版本在核心操作上均达到或快于 Go 对照，node footprint 与 Go 对齐。

```text
指标             1K C++ / Go       10K C++ / Go      100K C++ / Go
---------------  ----------------  ----------------  ----------------
Get              65 / 78.23 ns     120 / 134.4 ns    162 / 184.7 ns
PutReplace       84 / 91.46 ns     138 / 146.5 ns    177 / 195.9 ns
PutInsert        352 / 401.9 ns    492 / 568.1 ns    611 / 697.3 ns
Seek             103 / 113.2 ns    152 / 162.2 ns    193 / 213.4 ns
ReverseSeek      106 / 115.0 ns    157 / 164.9 ns    197 / 215.4 ns
DeleteHeavy      n/a               111 / 126.8 ns    140 / 155.4 ns
Footprint nodes  4 / 4             50 / 50           515 / 515
```

遍历性能：

```text
指标                         1K C++ / Go         10K C++ / Go        100K C++ / Go
---------------------------  ------------------  ------------------  ------------------
VisitFullSetOrdered          4 / 11.105 ns/item  4 / 11.198 ns/item  7 / 16.570 ns/item
VisitFullSetReverse          4 / 10.484 ns/item  4 / 10.558 ns/item  6 / 16.049 ns/item
```

benchmark 归档文件在本地 `.runtime/` 下，不进入 git：

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

`minweight_store` 是 C++ 版 KV store 主库，位于 `src/minweight_store/`。它将依赖并链接 `minpatricia`，用于承载原 Go 版 `minweight_store` 的完整存储引擎能力。

计划用途和场景：

- public KV API：`Put`、`Get`、`Delete`、`Len`、`SeekGE`、`SeekLE`、`Scan`、`ScanRange`、`ReverseScan`、`ReverseScanRange`。
- heap-backed in-memory store。
- WAL、manifest、mmap node store。
- primary index 与 secondary checkpoint index。
- flush、recovery、minor compaction、major compaction。
- SST backend 抽象与具体实现。
- `Env`/`Runtime` 抽象，用于适配标准线程、bthread/butex、I/O offload 或其他 coroutine/fiber runtime。

当前状态：`minweight_store` 尚未从 Go 版本翻译完成，README 中暂不展开实现细节。后续完成 heap-backed in-memory store、WAL、manifest、mmap node store、SST 和 compaction 后再补充 API、使用示例、durability 语义和性能结果。

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

## 开发规则

开始任务前先阅读 `docs/agents_read.md`，再同步 `docs/trackers/todos.md`、`docs/design/index.md` 和相关文档。新测试与被测试源码放在同一模块目录内。

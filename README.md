# minweight_store_cpp

`minweight_store_cpp` 来源于原 Go/golang 版本的 `minweight_store`。本仓库不是继续维护 Go 代码，而是使用 **C++17** 重新构建同一套核心能力，并面向后续嵌入 C++17/brpc/bthread 代码库的场景整理模块边界。

当前仓库采用 CMake 构建，默认 C++ 标准是 C++17，不依赖 C++20 `std::span`、concepts 或 coroutine 标准库能力。

原 Go 实现保存在：

- 远端分支：`golang`
- 本地临时快照：`golang_code/`，该目录被 `.gitignore` 忽略，仅用于重构期间 review 旧代码

对外目标保持两个 library：

```text
Target                         状态      用途
-----------------------------  --------  ------------------------------------------------
minpatricia                    Active    独立 ordered Patricia index，可单独被外部项目依赖
minpatricia::minpatricia       Active    minpatricia CMake alias target
minweight_store                Active    KV store 主库；WAL + snapshot checkpoint core 已实现
minweight_store::minweight_store Active  minweight_store CMake alias target
```

## 当前状态

```text
模块             当前能力
---------------  ------------------------------------------------------------------
minpatricia      C++17 完整翻译；支持 Get/Probe/Put/Delete/Retarget/seek/range scan
minweight_store  C++17 WAL + snapshot checkpoint store core；支持 Runtime 注入和 prefix recovery
```

`minweight_store` 现在已经是一个可用的 lightweight ordered KV core：数据写入 mmap WAL，checkpoint 时写入 generation snapshot 和 `MANIFEST`，恢复时先加载 snapshot，再 replay 当前 WAL generation 的连续合法 tail。它仍未达到原 Go 版完整 parity：mmap primary/secondary node checkpoint、Parquet/SST、minor/major compaction、dispatcher/logger 体系和 bthread/butex runtime adapter 还在 TODO 中。

## minpatricia

`minpatricia` 位于 `src/minpatricia/`。它是一个 C++17 ordered Patricia index，维护：

```text
byte string key -> opaque Position
```

`Position` 只是不透明位置句柄，由上层解释。它可以指向 WAL record、SST row、page id、外部 record handle 或其他存储位置。

### 主要特点

- 可以作为纯内存 ordered index 使用。
- 可以作为外部项目单独依赖，不需要链接 `minweight_store`。
- 使用仓库内置 `minpatricia::Span<const std::byte>` 作为 `ByteView`，兼容 C++17。
- `Index<RecordStore, NodeStore>` 使用 templates 静态分发，C++17 traits/static_assert 约束 store 形状。
- `NodePage` 固定 4096 字节，最多 339 个 rep，便于后续接入 mmap-backed node store。
- 不负责 WAL、manifest、fsync、mmap 生命周期、value 生命周期、锁或 runtime。

### minpatricia API

常用 public header：

```cpp
#include "minpatricia/minpatricia.h"
#include "minpatricia/index.h"
```

核心类型：

```text
类型 / 函数                         说明
----------------------------------  ------------------------------------------------
minpatricia::ByteView               C++17 span-like key view
minpatricia::Span<T>                仓库内置 span 替代品
minpatricia::Position               opaque record/node position
minpatricia::Status                 错误码
minpatricia::Result<T>              value-or-status
minpatricia::HeapRecordStore<V>     测试/轻量内存场景的 record store
minpatricia::HeapNodeStore          heap-backed node store
minpatricia::NewHeap<V>()           创建 heap-backed index
minpatricia::AsBytes(...)           string/string_view 到 ByteView 的零拷贝 view
```

`Index<RecordStore, NodeStore>` 主要 API：

```text
API                                                   说明
----------------------------------------------------  --------------------------------------------
Index::NewWithNodes(records, nodes)                   创建新 index，并初始化 root
Index::OpenWithNodes(records, nodes)                  打开已有 node store 中的 index
Len()                                                 live key 数
LiveNodes()                                           live node 数
Probe(key)                                            只查 route，不验证 record key
Get(key)                                              查找 key，并通过 RecordStore::Key 验证
Put(key, pos)                                         插入或替换 key -> pos
Delete(key)                                           删除 key
Retarget(key, old_pos, new_pos)                       将同一 key 的位置从 old_pos 改为 new_pos
Ascend(fn)                                            全量正向遍历
Descend(fn)                                           全量反向遍历
AscendRange(greater_or_equal, less_than, fn)          正向范围遍历：[ge, lt)
DescendRange(less_or_equal, greater_than, fn)         反向范围遍历：(gt, le]
AscendGreaterOrEqual(pivot, fn)                       从第一个 >= pivot 的 key 开始正向遍历
DescendLessOrEqual(pivot, fn)                         从第一个 <= pivot 的 key 开始反向遍历
AscendLessThan(pivot, fn)                             遍历 < pivot 的 key
DescendGreaterThan(pivot, fn)                         反向遍历 > pivot 的 key
```

### minpatricia 使用约束

- `minpatricia` 本身不加锁，不是线程安全容器；并发控制必须由上层完成。
- `ByteView` 是 view，不拥有 key 内存；调用方要保证调用期间 key buffer 有效。
- `RecordStore::Key(Position)` 必须能返回该 position 对应的 key，用于 `Get`、route 验证和遍历。
- `Position` 高位保留给 child tag，外部 record position 不得占用该 tag。
- `minpatricia` 不持久化 value，也不保证 crash recovery；它只维护 ordered index 结构。

### minpatricia 示例

```cpp
#include <cassert>
#include <string>

#include "minpatricia/minpatricia.h"

void ExampleMinpatricia() {
  auto heap = minpatricia::NewHeap<std::string>();
  assert(heap.ok());

  auto tree = heap.take_value();
  auto key = minpatricia::AsBytes("alice");
  minpatricia::Position pos = tree.records->Add(key, "value-1");

  auto put = tree.index.Put(key, pos);
  assert(put.ok());

  auto found = tree.index.Get(key);
  assert(found.ok() && found.value().found);
}
```

外部项目可实现自己的 `RecordStore` / `NodeStore`：

```cpp
auto index = minpatricia::Index<MyRecordStore, MyNodeStore>::NewWithNodes(records, nodes);
```

## minweight_store

`minweight_store` 位于 `src/minweight_store/`。它是 C++17 KV store 主库，依赖 `minpatricia` 作为 primary ordered index。

当前实现是 WAL + snapshot checkpoint 主链路，不是完整 Go parity。它适合当前目标：高性能、本地 KV、允许 crash 后丢失尾部写入，但要求恢复后状态顺序一致。

### 已实现能力

```text
能力                                      状态
----------------------------------------  --------
Open / Close                              已实现
Put / Get / Delete / Len                  已实现
Scan / ScanRange                          已实现
ReverseScan / ReverseScanRange            已实现
SeekGE / SeekLE                           已实现
mmap WAL append                           已实现
WAL CRC 校验                              已实现
WAL rollover                              已实现
Point-in-time prefix replay               已实现
Strict replay                             已实现
Runtime 注入                              已实现
StdRuntime                                已实现
MANIFEST                                  已实现，记录当前 WAL generation
generation snapshot checkpoint            已实现，Close 和 WAL rollover 阈值触发
旧 WAL generation / snapshot GC           已实现，checkpoint 发布后清理旧世代
mmap primary/secondary checkpoint         未实现
SST backend                               未实现
minor compaction / major compaction       未实现
bthread/butex Runtime adapter             未实现
```

### minweight_store API

常用 public header：

```cpp
#include "minweight_store/store.h"
```

核心类型：

```text
类型 / 函数                         说明
----------------------------------  ------------------------------------------------
minweight_store::Store              KV store 对象
minweight_store::Options            open/runtime/replay 配置
minweight_store::WALReplayPolicy    WAL replay 策略
minweight_store::Runtime            锁和 blocking I/O 注入边界
minweight_store::StdRuntime         标准线程 runtime 实现
minweight_store::Status             错误码
minweight_store::Result<T>          value-or-status
minweight_store::Item               scan/seek 返回的 key/value
minweight_store::AsBytes(...)       string/string_view 到 ByteView 的零拷贝 view
```

`Store` 主要 API：

```text
API                                               说明
------------------------------------------------  --------------------------------------------
Store::Open(path, options)                        打开或创建 disk-backed store
Store::New(options)                               创建临时 runtime store，主要用于轻量测试
Close()                                           关闭 store；默认 sync_on_close=true
Len()                                             当前 live key 数
Put(key, value)                                   写入或覆盖 key
Get(key)                                          查询 key，返回 value + found
Delete(key)                                       删除 key，返回是否实际删除
Scan(fn)                                          全量正向扫描
ScanRange(greater_or_equal, less_than, fn)        正向范围扫描：[ge, lt)
ReverseScan(fn)                                   全量反向扫描
ReverseScanRange(less_or_equal, greater_than, fn) 反向范围扫描：(gt, le]
SeekGE(key)                                       查找第一个 >= key 的 item
SeekLE(key)                                       查找第一个 <= key 的 item
```

`Options`：

```text
字段                  默认值                         说明
--------------------  -----------------------------  ------------------------------------------
wal_size              128 MiB                        单个 WAL segment 大小
wal_replay_policy     WALReplayPolicy::kPointInTime  默认恢复合法连续前缀
verify_index_on_read  false                          读路径额外验证 index position
sync_on_close         true                           Close 时是否同步 WAL
runtime               StdRuntime::Shared()           锁和 blocking I/O 实现
```

`Runtime`：

```text
API                     说明
----------------------  ------------------------------------------------
NewMutex()              创建互斥锁
NewRWMutex()            创建读写锁；store 主链路使用它保护 primary index
BlockingIO(name, task)  执行可能阻塞的文件系统操作
```

后续 bthread/butex 适配应实现同一个 `Runtime` 接口：锁使用 butex/bthread-aware primitive，`BlockingIO` 可投递到 pthread I/O worker pool 或 bthread-aware io_uring adapter。store public API 仍保持同步外观。

### minweight_store 示例

```cpp
#include <cassert>

#include "minweight_store/store.h"

void ExampleStore() {
  minweight_store::Options options;
  auto opened = minweight_store::Store::Open("/path/to/store", options);
  assert(opened.ok());

  auto store = opened.take_value();
  assert(store->Put(minweight_store::AsBytes("alice"),
                    minweight_store::AsBytes("value-1")).ok());

  auto got = store->Get(minweight_store::AsBytes("alice"));
  assert(got.ok() && got.value().found);

  auto seek = store->SeekGE(minweight_store::AsBytes("ali"));
  assert(seek.ok() && seek.value().found);

  assert(store->Close().ok());
}
```

### 写入与并发约束

当前写路径：

```text
Put:
  1. 获取 primary write lock。
  2. 校验 key size。
  3. 追加 put record 到 mmap WAL segment。
  4. 更新 WAL used offset。
  5. 调用 minpatricia::Index::Put 发布 primary index。
  6. 返回成功。

Delete:
  1. 获取 primary write lock。
  2. 查询 primary index。
  3. key 不存在则不写 tombstone，直接返回 false。
  4. 调用 minpatricia::Index::Delete，从进程内可见 index 移除 key。
  5. 追加 delete tombstone 到 mmap WAL segment。
  6. tombstone 追加失败时尝试恢复旧 position。
  7. checkpoint 触发成功后返回 true。
```

并发模型：

```text
读操作        持有 primary read lock，可并发读
写操作        持有 primary write lock，单 store 内写入串行
Scan          持有 primary read lock 直到 callback 停止或扫描结束
minpatricia   不自行加锁，由 store 层保护
```

如果需要高写并发，推荐在 store 外按业务 key 做 shard。当前单 store 不提供 RocksDB 式多 writer 并行写入。

### Checkpoint 与文件布局

当前 C++ 版没有移植 Go 版 mmap primary/secondary index checkpoint，而是使用更小的 generation snapshot：

```text
db/
  MANIFEST
  SNAPSHOT.<20-digit-generation>
  wal/
    <20-digit-generation>/
      <20-digit-file-no>.wal
```

checkpoint 顺序：

```text
1. 在 primary write lock 下收集 live key/value。
2. 写入并 fsync `SNAPSHOT.<next_generation>.tmp`，rename 为正式 snapshot。
3. 创建下一代 WAL 目录。
4. 写入并 fsync `MANIFEST.tmp`，rename 为 `MANIFEST`。
5. 切换内存 record store 到下一代空 WAL。
6. 用 snapshot-backed record 重新构建 heap index。
7. 清理旧 WAL generation 和旧 snapshot。
```

恢复顺序：

```text
1. 如果存在合法 MANIFEST，读取其中的 WAL generation。
2. 加载同 generation 的 SNAPSHOT。
3. 打开同 generation 的 WAL 目录并 replay 当前 WAL tail。
4. 清理旧 generation 文件。
5. 如果没有 MANIFEST，则从第 1 代 WAL 直接 replay。
```

这样 `MANIFEST` 不会指向被覆盖的固定名 snapshot。崩溃发生在 checkpoint 中途时，恢复要么继续使用旧 manifest+旧 snapshot，要么使用新 manifest+新 snapshot，不会混读两个世代。

### Crash-safe 语义

当前语义是 **weak durability + prefix recovery**：

```text
Put/Delete 返回：
  表示进程内已经可见。
  不表示该写入已经主动 msync/fsync 到磁盘。

crash recovery:
  从 WAL header 后顺序扫描。
  每条 record 校验 op、长度边界和 CRC。
  默认遇到第一条非法 record 时截断到 last_good_offset。
  非法 record 后面的内容不 replay。
```

因此恢复后的状态等价于写入序列的某个连续前缀：

```text
允许：丢失最后一段 suffix 写入。
禁止：跳过中间坏 record 后恢复更后面的 record。
```

当前支持两种 WAL replay 策略：

```text
策略                              说明
--------------------------------  ------------------------------------------------
WALReplayPolicy::kPointInTime     默认；恢复 CRC-valid 连续前缀，截断坏尾部
WALReplayPolicy::kStrict          遇到任意坏 record 时 Open 失败
```

这不是 RocksDB `WriteOptions.sync=true` 语义。如果业务要求 `Put` 返回即强持久化，需要后续增加 per-write WAL sync 或 group commit 语义。

### 当前未完成项

```text
能力                            影响
------------------------------  ------------------------------------------------
mmap node checkpoint            当前 snapshot 存 live KV；未持久化 minpatricia node page
SST backend                     尚不能把 immutable WAL compact 到 SST
minor/major compaction          尚无后台 GC/compaction
bthread/butex Runtime adapter   当前只有 StdRuntime，bthread 适配需要后续实现
Go 版 manifest append log       当前是单记录 MANIFEST rename，不是 1MiB append+compact log
Go 版 Parquet SST               当前没有 `sst/*.parquet` 外部可读数据层
```

对应到原 Go 代码，剩余翻译工作主要是：

```text
Go 模块/能力                         C++ 当前状态
-----------------------------------  ------------------------------------------------
mmap_node_store.go                   未移植；当前使用 heap node store + snapshot KV
flush.go / shadow checkpoint          未移植；当前用 generation snapshot 替代
manifest.go append/compact log        未移植；当前是单记录 MANIFEST rename
parquet_record_store.go               未移植；当前没有 Parquet/SST record store
minor_compaction.go                   未移植；当前 checkpoint 只能回收旧 WAL generation
major_compaction.go                   未移植；当前没有 SST merge/garbage cleanup
compaction_dispatcher.go              未移植；当前没有后台 compaction dispatcher
WALReplayBestEffort                   未移植；当前只有 strict 和 point-in-time
logger/options full surface           未移植；当前只保留核心 Options 和 Runtime
```

因此，当前 C++ 版是完备的 lightweight ordered KV core，但不是完整 Go 版 LSM/SST 引擎。如果数据集必须长期大于内存、需要 Parquet SST 落盘或后台 compaction，就必须继续移植 SST/compaction 体系。

## 性能结果

### minpatricia

当前 C++17 版 benchmark 使用 Go-generated fixed fixtures。2026-06-07 同机 Release 结果：

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

```text
指标                         1K C++ / Go         10K C++ / Go        100K C++ / Go
---------------------------  ------------------  ------------------  ------------------
VisitFullSetOrdered          4 / 11.105 ns/item  5 / 11.198 ns/item  7 / 16.570 ns/item
VisitFullSetReverse          4 / 10.484 ns/item  5 / 10.558 ns/item  8 / 16.049 ns/item
```

### minweight_store

当前 benchmark 对比 WAL/snapshot 主链路，key/value 生成方式一致，value size 为 64B，Go 版关闭 minor/major compaction 后运行。2026-06-07 同机 Release 结果：

```text
指标        1K C++ / Go      10K C++ / Go     100K C++ / Go
----------  ---------------  ---------------  ---------------
Put         573 / 790 ns     615 / 748 ns     619 / 753 ns
Get         137 / 206 ns     152 / 233 ns     158 / 226 ns
Scan        86 / 182 ns      87 / 198 ns      81 / 174 ns
SeekGE      211 / 319 ns     221 / 316 ns     234 / 348 ns
```

这组数据包含当前 C++ manifest/snapshot checkpoint 代码路径，但 benchmark 使用默认 128MiB WAL，正常主链路不会频繁触发 checkpoint。它不包含 Go 版 SST/compaction 的完整成本。

## 作为 submodule 使用

外部 CMake 项目可以直接：

```cmake
set(MINWEIGHT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MINWEIGHT_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

add_subdirectory(path/to/minweight_store_cpp)

target_link_libraries(your_target PRIVATE minpatricia::minpatricia)
# 或
target_link_libraries(your_target PRIVATE minweight_store::minweight_store)
```

如果只需要 ordered index，链接 `minpatricia::minpatricia` 即可；不会引入 `minweight_store`、WAL 或 runtime。

## 构建与验证

```bash
cmake -S . -B build -DCMAKE_CXX_STANDARD=17 -DCMAKE_BUILD_TYPE=Release -DMINWEIGHT_BUILD_TESTS=ON -DMINWEIGHT_BUILD_BENCHMARKS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

运行 benchmark：

```bash
./build/src/minpatricia/minpatricia_bench
MINPATRICIA_BENCH_LARGE=1 ./build/src/minpatricia/minpatricia_bench

./build/src/minweight_store/minweight_store_bench
MINWEIGHT_STORE_BENCH_LARGE=1 ./build/src/minweight_store/minweight_store_bench
```

## 文档入口

```text
docs/agents_read.md
docs/design/index.md
docs/design/minpatricia.md
docs/design/minweight_store.md
docs/trackers/todos.md
```

开始任务前先阅读 `docs/agents_read.md`，再同步 `docs/trackers/todos.md`、`docs/design/index.md` 和相关文档。新测试与被测试源码放在同一模块目录内。

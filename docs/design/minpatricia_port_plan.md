# minpatricia C++ 翻译计划

## 目标

把 Go 版 `github.com/JimChengLin/minpatricia` 翻译为当前仓库内的 C++17 `minpatricia` library，并保持它能被外部项目单独使用。

源码基线：

```text
模块     github.com/JimChengLin/minpatricia
版本     v0.0.0-20260531035544-2d1ddf70ce81
commit   2d1ddf70ce818228302912b81220b634218176b4
```

`minpatricia` 只负责 ordered index，不负责 WAL、manifest、mmap 生命周期、SST、锁和协程 runtime。它存储 opaque `Position` 和 child node id，不存储 key/value；key 由调用方实现的 `RecordStore` 提供，node page 由调用方实现的 `NodeStore` 提供。

本模块不做功能降级版或 MVP。实现可以按工程顺序分阶段提交，但 `minpatricia` 的完成标准是一次性补齐 Go 版所有 public API、split/merge/retarget/iterator 行为、model/golden/fuzz 类测试和性能基线。任何为了尽快跑通而替换掉 incremental route insert/delete、split、merge 或 retarget 语义的临时实现，都只能作为本地调试手段，不能进入主线。

性能目标是在 C++17 约束下把热路径做到接近极限：

- 默认性能路径避免 virtual dispatch。
- 热路径不发生 heap allocation。
- public API 使用零拷贝 key view。
- node page、route、rep 操作尽量 `constexpr`/inline。
- benchmark 目标不是只追平 Go，而是在相同算法和数据集上优先做到快于 Go。

## Go 实现要点

### 固定 node page

Go 版 `NodePage` 是 `node` 的别名，固定 4096 字节：

```text
字段       类型                         说明
---------  ---------------------------  --------------------------------
size       uint16                       当前 rep 数量
firstPos   Position                     子树最小 key 对应 position
lastPos    Position                     子树最大 key 对应 position
routes     [MaxNodeReps - 1]route       route 编码表
reps       [MaxNodeReps]rep             record position 或 child rep
padding    byte[]                       填充到 4096 字节
```

关键常量：

```text
NodeSize       4096
MaxNodeReps    339
childTag       1 << 63
```

`rep` 是 `uint64`。最高位为 0 时表示 record `Position`；最高位为 1 时表示 child node id。外部传入的 record `Position` 不能使用最高位。

### diff bit 语义

key 是 byte string。Go 版把每个 byte 看作 8 个 bit，再额外使用一个 terminator diff bit，因此一个 byte 对应 9 个 diff slot。

```text
diff = byte_index * 9              表示长度终止差异
diff = byte_index * 9 + bit_index  表示 byte 内 bit 差异，bit_index 范围 1..8
```

`MaxKeySize = (2^16 - 1) / 9`，因为 diff 使用 `uint16`。

### route 编码

Go 版不是每个 bit 一层普通 trie。每个 node 内先对相邻 rep 计算 diff，再用 diff 序列构造 Cartesian tree，并把 route 按 preorder 写入 `routes`。

`route.bits` 的 packed layout：

```text
bits[0..12]    byte index
bits[13..16]   bit operation，terminator 使用 8
bits[17..31]   leftCount
```

lookup 只在当前 node 的 route table 中移动，最后得到 rep slot。如果 slot 是 child rep，再进入 child node。

### API 语义

需要保持的 public API 行为：

```text
NewWithRecords
NewWithNodes
OpenWithNodes
Len
Get
Probe
Put
Delete
Retarget
Visit / Ascend
AscendRange / AscendLessThan / AscendGreaterOrEqual
Descend / DescendRange / DescendLessOrEqual / DescendGreaterThan
LiveNodes
```

`Probe` 只走 index，不读取 `RecordStore` 验证 key；`Get` 会读取 `RecordStore` 并确认 routed position 的 key 完全匹配。`Retarget` 只走 index，不读取 `RecordStore`，用于把同一个 key 的 old position 替换为 new position。

### 更新路径

- `Put` 命中同 key 时原地替换 rep，并传播 `firstPos`/`lastPos` 边界变化。
- `Put` 新 key 时先 route 到候选 leaf，计算新 key 与候选 key 的 diff，再找插入位置。
- node 未满时走 incremental route insert，避免整页重建。
- node 满时 split；若 parent 未满，优先把 split sibling 提升到 parent。
- `Delete` 命中 record 后 incremental route delete。
- child 为空时删除 child rep 并释放 node。
- child 可并入 parent 时直接 merge；极小 child 可与 sibling merge。
- root 只有一个 child 时压缩 root。

## C++ API 方案

命名空间：

```cpp
namespace minpatricia {
}
```

public header：

```text
src/minpatricia/include/minpatricia/
|-- byte_view.h
|-- index.h
|-- node_page.h
|-- node_store.h
|-- position.h
|-- record_store.h
`-- status.h
```

核心类型：

```cpp
using Position = std::uint64_t;

using ByteView = Span<const std::byte>;

template <class Store>
struct IsRecordStoreLike;

template <class Store>
struct IsNodeStoreLike;

template <class RecordStore, class NodeStore>
class Index;
```

默认 `Index<RecordStore, NodeStore>` 使用 C++17 templates 静态分发，并通过 traits/static_assert 约束调用方 store，避免 Go interface 风格在 C++ 中退化为每次 `Get`/`nodeByID` 的 virtual dispatch。若后续确实需要 ABI 稳定或插件式外部 store，可以额外提供 type-erased adapter，但不能作为默认性能路径。

`ByteView` 使用仓库内置 `minpatricia::Span<const std::byte>` 表达零拷贝 key view，避免依赖 C++20 `std::span`。为了调用方便，提供从 `std::string_view`、`Span<const std::uint8_t>` 构造 `ByteView` 的 helper，index 内部统一使用 `ByteView`。

`Status` / `Result<T>` 先在 `minpatricia` 内实现最小错误模型，错误码覆盖：

```text
EqualKeys
KeyTooLarge
UnsortedKeys
MissingKey
PositionTag
PositionMismatch
DuplicateKey
CorruptLayout
NilRecordStore
NilNodeStore
```

## NodePage C++ 布局

`NodePage` 必须显式约束 layout：

```cpp
struct Route {
  std::uint32_t bits;
};

struct NodePage {
  std::uint16_t size;
  std::byte reserved0[6];
  Position first_pos;
  Position last_pos;
  Route routes[MaxNodeReps - 1];
  std::uint64_t reps[MaxNodeReps];
  std::byte reserved1[8];
};

static_assert(sizeof(NodePage) == 4096);
static_assert(alignof(NodePage) == 8);
static_assert(offsetof(NodePage, first_pos) == 8);
static_assert(offsetof(NodePage, last_pos) == 16);
static_assert(offsetof(NodePage, routes) == 24);
static_assert(offsetof(NodePage, reps) == 1376);
```

Go 版 mmap node store 直接把 `node` struct 映射到文件，当前平台实际是 little-endian raw memory layout。C++ `minpatricia` 本身仍是内存 library；后续 `minweight_store` 做 mmap 持久化时，要明确 little-endian 与跨平台兼容策略。

## 翻译步骤

```text
阶段  内容                                      验证
----  ----------------------------------------  --------------------------------
1     CMake target 与 public header 骨架          能单独构建 minpatricia
2     Position/Rep/Route/NodePage/Status          static_assert layout + error tests
3     ByteView、diff bit、key compare             diff/prefix/MaxKeySize tests
4     HeapRecordStore、HeapNodeStore              allocator reuse + missing-key tests
5     route build 与 node rebuild                 Cartesian route + routeDiffs tests
6     Probe/Get/OpenWithNodes/Len/LiveNodes        empty/root/non-zero-root tests
7     Put replace 与 incremental insert           single-node correctness + no hot alloc
8     split/promoteSibling                        MaxNodeReps+1 和多节点 tests
9     Ascend/Descend/seek/range iterator          ordered/range/reverse tests
10    Delete/merge/compressRoot                   delete-heavy + route validation tests
11    Retarget                                    no RecordStore read + boundary propagation tests
12    model/fuzz/golden                           C++ map model + Go golden trace
```

这些阶段只是提交与验证顺序，不代表可以发布部分功能版本。`TODO-1` 只有在阶段 1 到 12 全部完成、性能基线达标后才算完成。

## 正确性测试计划

### 单元测试

按 Go 测试迁移以下类别：

```text
类别                         重点
---------------------------  ----------------------------------------------
Node layout                   sizeof、offset、MaxNodeReps 容量边界
Diff prefix semantics          terminator diff、MaxKeySize、equal key error
Heap stores                    node id reuse、record position reuse、nil key
Public error paths             nil store、too-large key、tagged position
Put/Get/Delete                 replace、missing key、sorted visit
Probe                          不读取 RecordStore、不验证 key
Retarget                       不读取 RecordStore、old position mismatch、边界缓存
Iterator                       Ascend/Descend/range/seek/early stop
OpenWithNodes                  打开已有 NodeStore，不重建 root
Multi-node                     split、promoteSibling、route validation
Delete-heavy                   merge、root compression、lookup 一致性
Corrupt paths                  route leftCount、missing child、empty child
```

### Model test

用 `std::map<std::string, Position>` 作为 oracle，随机执行：

```text
Put
Delete
Get
Probe + Get 对照
AscendGreaterOrEqual
DescendLessOrEqual
AscendRange
DescendRange
Retarget
```

固定 seed 覆盖 1K、10K、100K 操作规模。每隔固定步数检查：

- `Len()` 与 map size 一致。
- full ascend 顺序与 `std::map` 一致。
- full descend 顺序与反向 `std::map` 一致。
- 每个 live key 的 `Get` position 一致。

### Go golden trace

从 Go 版生成小型确定性 trace，建议落到 `src/minpatricia/testdata/`：

```text
golden_diff_cases.json       key pair -> cmp/diff
golden_ops_small.json        operation sequence -> final ordered keys
golden_seek_cases.json       dataset + pivot -> first GE / first LE
golden_layout_cases.json     selected route bits / leftCount / diff decode
```

golden 文件只保留小规模、人工可 review 的样本；大规模随机对照由 C++ model test 负责。

## 性能测试计划

### Go 基线

已在当前机器运行 Go 版 quick benchmark：

```bash
MIN_PATRICIA_BENCH_ONLY=minpatricia \
go test -run '^$' \
  -bench 'Benchmark(Get|PutReplace|PutInsert|Seek|ReverseSeek|Footprint)$' \
  -benchmem -benchtime=200ms

MIN_PATRICIA_BENCH_ONLY=minpatricia \
go test -run '^$' \
  -bench 'BenchmarkDeleteHeavy|BenchmarkDeleteHeavyFootprint' \
  -benchmem -benchtime=200ms
```

环境：

```text
goos    linux
goarch  amd64
cpu     INTEL(R) XEON(R) PLATINUM 8582C
```

结果摘要：

```text
Benchmark                         结果
--------------------------------  --------------------------------------------
Get/1K                            82.41 ns/op, 0 B/op, 0 allocs/op
Get/10K                           137.2 ns/op, 0 B/op, 0 allocs/op
PutReplace/1K                     97.21 ns/op, 0 B/op, 0 allocs/op
PutReplace/10K                    153.5 ns/op, 0 B/op, 0 allocs/op
PutInsert/1K                      437.8 ns/op, 50 B/op, 0 allocs/op
PutInsert/10K                     595.8 ns/op, 24 B/op, 0 allocs/op
Seek/1K                           118.2 ns/op, 0 B/op, 0 allocs/op
Seek/10K                          169.3 ns/op, 0 B/op, 0 allocs/op
ReverseSeek/1K                    117.2 ns/op, 0 B/op, 0 allocs/op
ReverseSeek/10K                   169.3 ns/op, 0 B/op, 0 allocs/op
DeleteHeavy/10K                   141.8 ns/op, 0 B/op, 0 allocs/op
Footprint/1K                      16.38 node_B/key, 4 nodes/op
Footprint/10K                     20.48 node_B/key, 50 nodes/op
DeleteHeavyFootprint/10K          25.79 node_B/live_key, 49 nodes/op
```

### C++ benchmark 覆盖

benchmark 从一开始就作为交付内容。优先使用 Google Benchmark；如果依赖引入当时未定案，则先实现轻量 `minpatricia_bench` 作为过渡，但必须保留同样的数据集、指标和输出字段。

必须覆盖：

```text
Benchmark             数据规模              指标
--------------------  --------------------  --------------------------------
Get                   1K, 10K, 100K         ns/op, alloc count
PutReplace            1K, 10K, 100K         ns/op, alloc count
PutInsert             1K, 10K, 100K         ns/item, node alloc count
Ascend full set        1K, 10K, 100K         ns/item
Descend full set       1K, 10K, 100K         ns/item
SeekGE                1K, 10K, 100K         ns/op
SeekLE                1K, 10K, 100K         ns/op
DeleteHeavy           10K, 100K             ns/op, merge count
Footprint             1K, 10K, 100K         live nodes, node_B/key
DeleteHeavyFootprint  10K, 100K             live nodes, node_B/live_key
```

### 性能门槛

完成标准：

- `Get`、`Probe`、`SeekGE`、`SeekLE`、full scan、`PutReplace`、`DeleteHeavy` 热路径不发生 heap allocation。
- `NodePage` footprint 与 Go 版一致，`node_B/key` 在相同数据集上偏差不超过 5%。
- C++ benchmark 不慢于 Go 版；若不能快于 Go，需要给出明确 profiling 证据，说明瓶颈来自相同算法本身而不是 C++ 实现开销。
- 对 `Get`、`Probe`、route lookup、`PutReplace`、`Retarget`、seek hot path 使用 template 静态分发性能路径，默认不引入 virtual call。
- Release benchmark 使用 `-O3 -DNDEBUG`，Debug 结果不作为性能判断。
- 本地极限 benchmark 可启用 `-march=native`、LTO 和 PGO；通用 release 包保留可移植编译配置。

工具目标：

- 引入 Google Benchmark 或等价框架，提供稳定多轮结果和方差。
- 和 `std::map`、可选 `absl::btree_map` 做对比，但对比项不是 correctness gate。
- 建立 `.runtime/benchmarks/` 输出目录，保存 Go baseline、C++ baseline 和后续回归结果。

### C++17 兼容实现点

当前实现保持 C++17 兼容，同时保留零拷贝和静态分发的性能路径：

- `minpatricia::Span<const std::byte>` 表达 key view，避免复制。
- C++17 traits/static_assert 约束 `RecordStore`/`NodeStore`，让 `Index` 默认走静态分发。
- 自实现 8-bit leading-zero helper 计算 diff bit，避免依赖 C++20 `<bit>`。
- `constexpr` 常量和 helper 锁定 route bit packing、layout byte 计算和 child tag 操作。
- `std::array` 和固定 stack buffer 承载常见路径，避免小对象 heap 分配。
- 后续 mmap 持久化 layout 的 endian 策略由 `minweight_store` 明确实现，不依赖 C++20 `std::endian`。

## 当前 C++17 实现状态

截至 2026-06-07，`src/minpatricia/` 已落地完整 C++17 兼容核心实现：

- `Index<RecordStore, NodeStore>` 使用 templates + traits，默认热路径静态分发。
- public API 覆盖 `NewWithRecords`、`NewWithNodes`、`OpenWithNodes`、`Len`、`Get`、`Probe`、`Put`、`Delete`、`Retarget`、`Visit/Ascend`、所有 Ascend/Descend range/seek 变体、`LiveNodes`。
- 已移植 Go 版核心算法：diff bit、Cartesian route rebuild、incremental route insert/delete、split/promoteSibling、delete merge、root compression、iterator path、Retarget boundary propagation。
- `NodePage` 保持 4096 字节，`Rep` 8 字节，`Route` 4 字节，并用 static_assert 锁定 offset。
- `HeapRecordStore<V>` 和 `HeapNodeStore` 可作为默认内存实现；后续 mmap node store 可复用 `NodeStoreLike` 边界。

当前 C++ 测试覆盖：

- layout/diff/route/store/API/error path。
- single-node Put/Get/Delete/Visit/Probe/Retarget。
- Ascend/Descend/range/seek/early stop。
- 2500 key multi-node split/replace/delete against `std::map`。
- child boundary replace/Retarget 后的父 boundary cache 传播。
- 3000 step deterministic model test，覆盖 put/delete/get/seek/range。
- non-zero root node store 与 `OpenWithNodes`。
- Go fuzz seed 回放，包括 empty/NUL-byte key 和 `MaxNodeReps + 1` split seed。
- delete-all route consistency 与 100K delete-heavy lookup consistency。
- 递归 route validation：`NodePage::RouteDiffs()` 必须等于按 record key 重新计算出的 diff 序列。

完成状态：

- `TODO-1` 已完成并移动到 `docs/trackers/resolved.md`。
- 当前采用无第三方依赖的等价 benchmark harness，而不是引入 Google Benchmark。
- benchmark 使用 Go-generated fixed fixtures：`bench_keys_1K.tsv`、`bench_keys_10K.tsv`、`bench_keys_100K.tsv`。
- 当前 golden trace 采用 TSV/line 格式，避免引入 JSON 依赖；fixture 生成脚本保留在 `src/minpatricia/testdata/gen_bench_fixtures.go`。

当前同机 C++17 Release 性能对照：

```text
Benchmark             Go minpatricia       C++ minpatricia
--------------------  ------------------   ----------------
Get/1K                78.23 ns/op          62 ns/op
Get/10K               134.4 ns/op          117 ns/op
Get/100K              184.7 ns/op          160 ns/op
PutReplace/1K         91.46 ns/op          94 ns/op
PutReplace/10K        146.5 ns/op          143 ns/op
PutReplace/100K       195.9 ns/op          191 ns/op
PutInsert/1K          401.9 ns/op          423 ns/op
PutInsert/10K         568.1 ns/op          579 ns/op
PutInsert/100K        697.3 ns/op          691 ns/op
VisitFullSetOrdered/1K 11.105 ns/item      4 ns/item
VisitFullSetOrdered/10K 11.198 ns/item     5 ns/item
VisitFullSetOrdered/100K 16.570 ns/item    7 ns/item
VisitFullSetReverse/1K 10.484 ns/item      4 ns/item
VisitFullSetReverse/10K 10.558 ns/item     5 ns/item
VisitFullSetReverse/100K 16.049 ns/item    8 ns/item
Seek/1K               113.2 ns/op          100 ns/op
Seek/10K              162.2 ns/op          153 ns/op
Seek/100K             213.4 ns/op          191 ns/op
ReverseSeek/1K        115.0 ns/op          109 ns/op
ReverseSeek/10K       164.9 ns/op          156 ns/op
ReverseSeek/100K      215.4 ns/op          192 ns/op
DeleteHeavy/10K       126.8 ns/op          122 ns/op
DeleteHeavy/100K      155.4 ns/op          150 ns/op
Footprint/1K          16.38 node_B/key     16.384 node_B/key
Footprint/10K         20.48 node_B/key     20.48 node_B/key
Footprint/100K        21.09 node_B/key     21.0944 node_B/key
DeleteHeavyFootprint/10K 25.79 node_B/live_key 25.7941 node_B/live_key
DeleteHeavyFootprint/100K 30.79 node_B/live_key 30.7861 node_B/live_key
```

Go 与 C++ benchmark 使用同源 fixed fixture。C++ benchmark 中的 borrowed `RecordStore` 仅用于隔离 index 本体性能；默认 `HeapRecordStore<V>` 的拥有式 key copy 仍由 UT 覆盖。

### 重点风险

- C++ struct padding 与 Go raw mmap layout 不一致。
- `ByteView` 生命周期错误，尤其 iterator callback 中 key view 来自 `RecordStore`。
- route incremental insert/delete 细节错误，短期可能被 full rebuild 掩盖但会损害性能。
- `Retarget` 误读 `RecordStore`，破坏 Go 版用于 SST install retarget 的性能语义。
- model test 只测 full order，不测 seek/range 边界，会漏掉 `insertSlotFromPath` 类错误。

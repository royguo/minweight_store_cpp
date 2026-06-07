# minpatricia 设计说明

## 模块定位

`minpatricia` 是 `minweight_store` 使用的有序索引库，也可以作为独立 C++17 library 被外部项目直接链接。它的职责只包括：

- 维护 byte string key 的全序索引。
- 把 key 映射到调用方提供的 opaque `Position`。
- 支持 `Get`、`Probe`、`Put`、`Delete`、`Retarget`、正向/反向遍历和范围 seek。
- 管理 4KB `NodePage` 中的 Patricia route、record rep、child rep、split、merge 和 root compression。

它不负责以下内容：

- value 存储和 value 生命周期。
- WAL、manifest、fsync、mmap 文件生命周期。
- 并发控制、事务隔离、锁、协程 runtime。
- key/value 的序列化协议。

调用方需要提供两个 store：

```text
组件           职责
-------------  ------------------------------------------------------------
RecordStore    根据 `Position` 返回 key，用于索引验证、比较和遍历
NodeStore      提供 root node、按 id 读取 node、分配/释放 node、统计 live node
```

这种拆分让 `minpatricia` 可以同时服务两类场景：

- 在 `minweight_store` 内部，`Position` 指向 WAL/SST/memtable 中的 record 位置，`NodeStore` 后续可以落到 mmap 或 page cache。
- 外部项目只想使用 ordered Patricia index 时，可以使用库内 `HeapRecordStore` 和 `HeapNodeStore`，也可以接入自己的 record/node 后端。

## 公开接口

核心头文件是：

```text
src/minpatricia/include/minpatricia/minpatricia.h
```

核心类型位于 `namespace minpatricia`：

```text
类型                                      说明
----------------------------------------  ----------------------------------------
Position                                  record 位置，最高位保留给 child tag
ByteView                                  `minpatricia::Span<const std::byte>` 零拷贝 key view
Status / Result<T>                        错误模型
HeapRecordStore<V>                        测试和轻量使用场景的拥有式 record store
HeapNodeStore                             heap-backed node store
Index<RecordStore, NodeStore>             索引主体
```

`Index` 的主要 API：

```text
API                                       语义
----------------------------------------  ----------------------------------------
NewWithNodes(records, nodes)              初始化一个新的 index，必要时清空 root
OpenWithNodes(records, nodes)             打开已有 node store，不重建 root
Len()                                     返回 record 数量
LiveNodes()                               返回 node store 中 live node 数量
Probe(key)                                只走索引 route，返回候选 position 和 found 标志
Get(key)                                  route 后读取 RecordStore，确认 key 完全匹配
Put(key, pos)                             插入或替换 key 对应的 position
Delete(key)                               删除 key，返回旧 position 和 found 标志
Retarget(key, old_pos, new_pos)           不读取 RecordStore，把同 key 的 old position 替换成 new position
Visit(fn) / Ascend(fn)                    按 key 升序遍历
AscendRange(lo, hi, fn)                   遍历 `[lo, hi)`
AscendLessThan(pivot, fn)                 遍历 `< pivot`
AscendGreaterOrEqual(pivot, fn)           遍历 `>= pivot`
Descend(fn)                               按 key 降序遍历
DescendRange(hi, lo, fn)                  遍历 `(lo, hi]`
DescendLessOrEqual(pivot, fn)             遍历 `<= pivot`
DescendGreaterThan(pivot, fn)             遍历 `> pivot`
```

`Probe` 和 `Get` 的差异非常重要：

- `Probe` 只依赖 node route，不读取 `RecordStore` 验证 key。它适合调用方已经知道 route 一定有效，或者只想得到候选 position 的低成本场景。
- `Get` 会读取 routed position 的 key，并确认它和查询 key 完全相等。普通查询应优先使用 `Get`。

`Retarget` 也只走索引 route，不读取 `RecordStore`。它用于调用方在 compact、rewrite、WAL replay 或 record relocation 后，把同一个 key 的位置从 `old_pos` 替换为 `new_pos`。调用方必须保证 `new_pos` 指向同一个 key，否则后续 `Get` 会在验证时暴露不一致。

## 使用方式

### CMake 链接

仓库会导出 `minpatricia` 和别名 target：

```cmake
target_link_libraries(your_target PRIVATE minpatricia::minpatricia)
```

### 使用 heap store

`NewHeap<V>()` 适合测试、样例和轻量 in-memory 使用：

```cpp
#include <iostream>
#include <string>

#include "minpatricia/minpatricia.h"

int main() {
  auto heap = minpatricia::NewHeap<std::string>();
  if (!heap.ok()) {
    return 1;
  }

  auto tree = heap.take_value();
  const auto key = minpatricia::AsBytes("alice");
  const minpatricia::Position pos = tree.records->Add(key, "value-1");

  auto put = tree.index.Put(key, pos);
  if (!put.ok()) {
    return 1;
  }

  auto found = tree.index.Get(key);
  if (!found.ok() || !found.value().found) {
    return 1;
  }

  auto value = tree.records->Value(found.value().position);
  if (!value.ok()) {
    return 1;
  }

  std::cout << *value.value() << "\n";
  return 0;
}
```

### 接入自定义 store

性能路径使用 C++17 templates 和 traits 静态分发，不通过 virtual interface。自定义 `RecordStore` 需要提供：

```cpp
class MyRecordStore {
 public:
  minpatricia::Result<minpatricia::ByteView> Key(minpatricia::Position pos);
};
```

自定义 `NodeStore` 需要提供：

```cpp
class MyNodeStore {
 public:
  std::uint64_t Root() const;
  minpatricia::Result<minpatricia::NodePage*> Get(std::uint64_t id);
  minpatricia::Result<minpatricia::NodeAlloc> Alloc();
  minpatricia::Status Free(std::uint64_t id);
  int LiveNodes() const;
};
```

然后构造 index：

```cpp
MyRecordStore records;
MyNodeStore nodes;

auto index = minpatricia::Index<MyRecordStore, MyNodeStore>::NewWithNodes(records, nodes);
if (!index.ok()) {
  return index.status();
}
```

打开已有 node store 时使用 `OpenWithNodes`：

```cpp
auto index = minpatricia::Index<MyRecordStore, MyNodeStore>::OpenWithNodes(records, nodes);
```

`OpenWithNodes` 不会重建 root，适合持久化 node store 重新打开。调用方仍需保证 `RecordStore` 中所有 `Position` 能按旧 node 内容读回正确 key。

## 数据结构

### Position 和 Rep

`Position` 是 `uint64_t`。最高位保留给 child tag：

```text
childTag = 1 << 63
```

node 内部使用 `Rep` 表示一个 slot：

```text
最高位  含义
------  ------------------------------------------------------------
0       record rep，低 63 bit 是调用方 record `Position`
1       child rep，低 63 bit 是 child node id
```

因此调用方传入的 record `Position` 不能设置最高位。`Put` 和 `Retarget` 会校验这一点，并在非法 position 上返回 `PositionTag`。

### NodePage

每个 node 固定 4096 字节，最多容纳 339 个 rep：

```text
字段        说明
----------  ------------------------------------------------------------
size        当前 rep 数量
first_pos   当前 node 子树的最小 key 对应 position
last_pos    当前 node 子树的最大 key 对应 position
routes      `size - 1` 个 packed route
reps        record rep 或 child rep
padding     填充到 4096 字节
```

当前 C++ layout 通过 `static_assert` 固定：

```text
sizeof(NodePage)              4096
alignof(NodePage)             8
offsetof(first_pos)           8
offsetof(last_pos)            16
offsetof(routes)              24
offsetof(reps)                1376
```

`first_pos` 和 `last_pos` 是边界缓存。它们让父节点在比较 child 边界、seek、split 和 merge 时避免扫描整棵子树。所有替换、插入、删除、Retarget、merge 和 root compression 都必须同步传播边界变化。

### Route

`minpatricia` 不是每个 bit 一层的普通 trie。一个 `NodePage` 内部把多个相邻 key 的分歧点压缩成 route table。route packed layout：

```text
bit range      字段
-------------  ------------------------------------------------------------
0..12          byte index
13..16         bit operation，terminator 使用 8
17..31         leftCount
```

route 的数量始终是 `size - 1`。lookup 在一个 node 内根据 route table 移动，最终得到 leaf slot。如果该 slot 是 record rep，则完成定位；如果是 child rep，则进入 child node 继续查找。

## 算法原理

### Diff bit

key 是 byte string。算法把每个 byte 表示成 8 个普通 bit，再额外加入一个 terminator diff slot。因此每个 byte 有 9 个 diff slot：

```text
diff = byte_index * 9              表示长度终止差异
diff = byte_index * 9 + bit_index  表示 byte 内 bit 差异，bit_index 范围 1..8
```

这个设计让普通前缀关系可以和 byte 内 bit 差异统一处理。例如：

```text
left    "abc"
right   "abcd"
diff    3 * 9
```

`MaxKeySize = (2^16 - 1) / 9`，因为 diff 使用 `uint16_t` 保存。

### Route 构造

对一个 node 内按 key 排序的 `reps`，先计算相邻 rep 的 diff：

```text
rep[0] rep[1] rep[2] ... rep[n-1]
   d0     d1     d2  ...   d[n-2]
```

然后用 diff 序列构造 Cartesian tree，并按 preorder 写入 `routes`。核心性质是：

- 每个 route 代表一个分歧点。
- `leftCount` 表示该 route 左子树覆盖的 rep 数量。
- 查询 key 在 route 上取 bit，决定进入左侧或右侧区间。
- 直到区间收敛为单个 rep slot。

这种布局把 node 内查找压缩成 compact route table 访问，减少指针跳转和 cache miss。

### Put

`Put(key, pos)` 的流程：

```text
1. 校验 key 大小和 position tag。
2. 如果 root 为空，直接插入第一个 record rep。
3. 从 root route 到候选 leaf。
4. 读取候选 leaf 的 key。
5. 如果 key 相等，原地替换 position，并传播 first/last 边界变化。
6. 如果 key 不等，计算新 key 和候选 key 的 diff，确定插入路径与 slot。
7. 如果目标 node 未满，执行 incremental route insert。
8. 如果目标 node 已满，选择 split range，分裂 node。
9. 如果 parent 可容纳 sibling，把 sibling 提升到 parent。
10. 必要时继续向上 split，root 满时创建新的 root 结构。
```

热路径上的 replace 和普通 insert 不重建整棵树。单 node 未满时只增量调整该 node 的 route table。

### Delete

`Delete(key)` 的流程：

```text
1. 从 root route 到候选 leaf。
2. 如果候选不是 record 或 key 不匹配，返回 not found。
3. 删除 record rep，并执行 incremental route delete。
4. 如果 child node 变空，释放 child 并删除父节点 child rep。
5. 如果 child 可以并入 parent，则 merge 到 parent 并释放 child。
6. 小 child 可以和 sibling 合并，减少稀疏节点。
7. root 只剩一个 child 时压缩 root。
```

删除路径的关键目标是保持以下不变量：

- 每个 node 的 `reps` 仍按 key 升序。
- `routes` 与相邻 rep diff 一致。
- `first_pos` 和 `last_pos` 与子树真实边界一致。
- `Len()` 和 `LiveNodes()` 与 record/node 状态一致。

### Retarget

`Retarget(key, old_pos, new_pos)` 用于位置重写。它不读取 `RecordStore`，只通过 route 定位候选 slot：

```text
1. 校验 old/new position tag。
2. route 到候选 leaf。
3. leaf 必须是 record rep，且 position 等于 old_pos。
4. 把 slot 替换为 new_pos。
5. 如果替换影响 first/last 边界，向父节点传播。
```

这个 API 是为了 storage 层 relocation 准备的。例如 compaction 把某个 key/value 从旧 SST 位置迁移到新 SST 位置时，可以先保证新位置内容可读，再调用 `Retarget` 更新索引位置。

### Iterator 和 seek

iterator 使用内部 path stack 保存从 root 到当前 record 的路径。正向遍历从 leftmost record 开始，反向遍历从 rightmost record 开始。seek API 会先找到边界位置，再沿 path 做 `NextPath` 或 `PrevPath`。

范围 API 的边界语义：

```text
API                         范围
--------------------------  ----------------
AscendRange(lo, hi)         [lo, hi)
AscendLessThan(pivot)       [begin, pivot)
AscendGreaterOrEqual(pivot) [pivot, end)
DescendRange(hi, lo)        (lo, hi]
DescendLessOrEqual(pivot)   key <= pivot，按降序访问
DescendGreaterThan(pivot)   key > pivot，按降序访问
```

visitor 返回 `false` 时提前停止遍历，返回 `true` 时继续。

## 正确性不变量

实现和测试持续校验以下不变量：

```text
不变量                         说明
-----------------------------  ------------------------------------------------
NodePage layout 固定            后续 mmap node store 可以复用相同页布局
Record position 无 child tag     最高位只允许 child rep 使用
key 大小不超过 MaxKeySize        diff 使用 uint16_t 保存
node 内 reps 按 key 有序          route 构建和 range iterator 的基础
routes 等于相邻 rep diff         route table 不能和 key 序列漂移
first/last 边界准确              parent routing、seek、split、merge 依赖边界缓存
root 可压缩                      删除后避免只剩单 child 的多余层级
Retarget 不读取 RecordStore      storage relocation 不能被旧 record 可读性阻塞
```

## 性能设计

当前 C++ 实现选择 C++17 作为标准，主要性能策略：

- `Index<RecordStore, NodeStore>` 使用 templates 静态分发，并用 C++17 traits 校验 store 形状，默认热路径没有 virtual dispatch。
- key 使用 `ByteView` 零拷贝传入，索引内部不拥有 key。
- `NodePage` 固定 4KB，route 和 rep 连续存放，提升 cache locality。
- `Rep` 和 `Route` 是小整数 packed 类型，常用操作 inline。
- lookup、seek、iterator 复用 path 和页内 route，不分配 heap。
- 普通 insert/delete 使用 incremental route insert/delete，避免无意义整页重建。
- benchmark 使用 Go-generated fixed fixtures，保证 C++ 和 Go 对照 keyset 一致。

2026-06-07 C++17 Release benchmark 结果显示，C++ 版本在 1K/10K/100K 上的核心指标保持接近或快于 Go 对照，node footprint 与 Go 对齐：

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

benchmark 归档文件：

```text
.runtime/benchmarks/2026-06-05/cpp_minpatricia_bench_large.txt
.runtime/benchmarks/2026-06-05/go_minpatricia_bench_large.txt
```

`.runtime/` 是本地运行归档目录，不进入 git。

## 测试覆盖

覆盖入口：

```text
src/minpatricia/minpatricia_test.cc
src/minpatricia/minpatricia_bench.cc
design/minpatricia_go_ut_coverage.md
```

当前测试覆盖：

- Go `index_test.go`、`coverage_test.go`、`index_fuzz_test.go` 的主要 public 行为和错误路径。
- Go fuzz seed 回放。
- map model 随机操作对照。
- 100K delete-heavy lookup consistency。
- `NodePage::RouteDiffs()` 与根据 record key 重新计算的 diff 序列对照。
- fixed golden trace：diff、route、seek、small ops。
- benchmark 覆盖 Go `bench_test.go` 中的 Get、PutReplace、PutInsert、Visit、Seek、ReverseSeek、DeleteHeavy、Footprint。

## 和 minweight_store 的集成边界

`minpatricia` 进入 `minweight_store` 后，推荐边界如下：

```text
层级               责任
-----------------  ------------------------------------------------------------
minpatricia         只维护 key -> Position 的 ordered index
memtable/record     负责 key/value 拥有权和 Position 分配
WAL                 负责 Put/Delete/Retarget 前后的崩溃恢复语义
manifest/SST        负责持久化文件、版本和 compaction
Env/runtime         负责 mmap、write、fsync、后台任务和协程/fiber 适配
```

写入顺序不能由 `minpatricia` 自己保证。典型 store 写路径应由上层控制：

```text
1. 写 WAL，确保 record mutation 可恢复。
2. 写入或更新 record storage，拿到新的 Position。
3. 调用 minpatricia::Index::Put 或 Retarget 更新内存索引。
4. 后台 flush/compaction 时按 store 的 durability 协议推进。
```

如果后续 `NodeStore` 改为 mmap 持久化，需要额外明确：

- `NodePage` raw layout 的 endian 策略。
- node page 写入和 WAL/manifest 的原子性关系。
- crash 后是重放 WAL 重建 index，还是直接信任 mmap node image。
- `Retarget` 与 compaction commit point 的顺序。

这些内容属于 `minweight_store` 的 durability 设计，不应下沉到 `minpatricia`。

## 并发约束

当前 `Index` 不做内部同步。调用方应按使用场景提供并发控制：

- 单 writer 或外部 mutex 保护 `Put`、`Delete`、`Retarget`。
- 读写并发需要上层提供 RW lock、copy-on-write 或版本化快照。
- `RecordStore` 和 `NodeStore` 的生命周期必须覆盖 `Index` 生命周期。
- 遍历期间如果允许并发修改，必须由上层定义 iterator 看到的版本语义。

这个约束保持了 `minpatricia` 的热路径简单，也避免把 store 层的事务和协程模型提前绑定到索引库。

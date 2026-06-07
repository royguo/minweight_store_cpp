# minweight_store 设计说明

## 当前状态

`minweight_store` 是仓库中的 C++17 KV store 主库，位于 `src/minweight_store/`，生成 public library target：

```text
minweight_store
minweight_store::minweight_store
```

当前实现已经具备 WAL + snapshot checkpoint 主链路：

```text
能力                                      状态
----------------------------------------  --------
Runtime 注入                              已实现
StdRuntime                                已实现
Open / Close                              已实现
Put / Get / Delete / Len                  已实现
Scan / ScanRange                          已实现
ReverseScan / ReverseScanRange            已实现
SeekGE / SeekLE                           已实现
mmap WAL append                           已实现
WAL CRC 校验                              已实现
Point-in-time prefix replay               已实现
Strict replay                             已实现
WAL rollover                              已实现
MANIFEST                                  已实现，记录当前 WAL generation
generation snapshot checkpoint            已实现，保存 live KV snapshot
旧 WAL generation / snapshot GC           已实现
mmap node store checkpoint                未实现
secondary checkpoint index                未实现
SST / minor compaction / major compaction 未实现
```

这不是 RocksDB `sync=true` 语义。当前写路径遵循用户确认的高性能语义：`Put` / `Delete` 返回表示进程内可见，不表示每次写入已经主动 `msync` 或 `fsync`。

## Public API

公共头文件：

```cpp
#include <minweight_store/store.h>
```

主要 API：

```cpp
auto opened = minweight_store::Store::Open(path, options);
auto store = opened.take_value();

store->Put(minweight_store::AsBytes("key"), minweight_store::AsBytes("value"));
auto got = store->Get(minweight_store::AsBytes("key"));
store->Delete(minweight_store::AsBytes("key"));

store->SeekGE(minweight_store::AsBytes("key"));
store->SeekLE(minweight_store::AsBytes("key"));
store->Scan(callback);
store->ReverseScan(callback);
store->Close();
```

API 保持同步外观，便于被普通线程、bthread 或其他 fiber/coroutine 上层直接调用。

## Runtime 边界

`Runtime` 是 `minweight_store` 的调度与阻塞 I/O 注入边界，不作为第三个公开 library target。当前 public 抽象包括：

```text
接口                  用途
--------------------  ----------------------------------------
NewMutex              创建普通互斥锁
NewRWMutex            创建读写锁
BlockingIO            执行可能阻塞的文件系统操作
```

`StdRuntime` 使用标准库锁，并在线执行 `BlockingIO`。未来 bthread/butex 版本应在同一接口下替换：

```text
能力                  bthread runtime 建议
--------------------  ------------------------------------------------
NewMutex              butex 或 bthread-aware mutex
NewRWMutex            bthread-aware rwlock
BlockingIO            pthread I/O worker pool 或 bthread-aware io_uring
```

这样 store 主链路不需要暴露 coroutine/awaitable API；上层在 bthread 中调用时看到的仍是同步接口。

## 写入语义

当前 `Put` 写入顺序：

```text
1. 获取 primary write lock。
2. 校验 key size。
3. 追加 WAL put record 到 mmap segment。
4. 更新 WAL used offset。
5. 调用 minpatricia::Index::Put 发布 primary index。
6. 释放 primary lock。
7. 返回成功。
```

当前 `Delete` 写入顺序：

```text
1. 获取 primary write lock。
2. 从 primary index 查询当前 key。
3. 如果 key 不存在，不写 tombstone，直接返回 false。
4. 调用 minpatricia::Index::Delete，从进程内可见 index 移除 key。
5. 追加 WAL delete record 到 mmap segment。
6. 如果 tombstone 追加失败，尝试把旧 position 放回 index。
7. 释放 primary lock。
8. 返回删除结果。
```

如果 WAL record 已经接受，但 index 更新失败，store 会进入 fatal 状态。这样避免继续在 WAL 与 index 已经分叉的状态下服务请求。`Delete` 在 tombstone 追加前已经从 index 移除；如果 tombstone 追加失败且旧 position 无法恢复，也会进入 fatal 状态。

## WAL 格式

当前 C++ 版复用 Go 版 WAL 二进制布局：

```text
字段                        值
--------------------------  ----------------------------------------
header size                 4096
magic                       "MWWAL01\0"
version                     1
record header size          13
position encoding           file_no << 30 | offset
record ops                  Put = 1, Delete = 2
CRC                         IEEE CRC32 over op/len fields and payload
```

每条 record 的 payload：

```text
op | key_len | value_len | crc | key | value
```

`Delete` record 的 `value_len` 必须为 0。

## Checkpoint 与文件布局

当前 C++ 版使用 generation snapshot，而不是 Go 版 mmap primary/secondary node checkpoint：

```text
db/
  MANIFEST
  SNAPSHOT.<20-digit-generation>
  wal/
    <20-digit-generation>/
      <20-digit-file-no>.wal
```

`MANIFEST` 是固定长度单记录文件，记录当前 WAL generation，并带 CRC。`SNAPSHOT.<generation>` 保存该 checkpoint 时刻的 live key/value，并在 header 中记录 generation；每条 snapshot record 也带 CRC。

checkpoint 发布顺序：

```text
1. 持有 primary write lock，按 index 顺序收集 live items。
2. 写入、fsync 并 rename `SNAPSHOT.<next_generation>`。
3. 创建下一代 WAL 目录。
4. 写入、fsync 并 rename `MANIFEST`。
5. 关闭旧 WAL mmap，打开下一代空 WAL。
6. 从 snapshot-backed record 重建 heap minpatricia index。
7. 删除旧 WAL generation 和旧 snapshot。
```

这个顺序避免固定名 snapshot 覆盖带来的 crash 窗口：崩溃在 manifest rename 前会继续使用旧 generation；崩溃在 manifest rename 后会使用新 generation。

## 恢复语义

默认 replay policy 是 `WALReplayPolicy::kPointInTime`：

```text
从 WAL header 后开始顺序扫描。
每条 record 检查 op、长度边界和 CRC。
遇到第一个非法 record 时，截断 WAL used 到 last_good_offset。
非法 record 后面的所有内容都不 replay。
```

因此 crash recovery 后状态等价于写入序列的某个连续前缀。允许丢失尾部写入，不允许跳过中间坏记录后恢复后续写入。

`WALReplayPolicy::kStrict` 用于测试和更严格启动策略：遇到任意坏 record 时 `Open` 失败。

有合法 manifest 时，启动先加载同 generation 的 snapshot，再 replay 该 generation 的 WAL tail。因此启动成本与 snapshot 大小和当前 WAL tail 相关，不再依赖所有历史 WAL。没有 manifest 时，启动从第 1 代 WAL 直接 replay。

当前仍没有实现 Go 版 primary/secondary mmap node checkpoint；snapshot 保存的是 live KV，而不是 minpatricia node page。因此它是轻量 KV core 的 checkpoint，不是 Go 版 checkpoint 机制的完整翻译。

## 并发语义

当前 C++ 版沿用 Go 版 coarse lock 模型：

```text
读操作        持有 primary read lock
写操作        持有 primary write lock
Scan          持有 primary read lock 直到 callback 停止或扫描结束
```

`minpatricia` 自身仍然不负责加锁。并发控制属于 store 层。

这种模型适合低写冲突、读多写少、或外部按 shard 拆分的场景。如果需要高并发多 writer，推荐在 store 外做 shard，而不是在 `minpatricia` node page 内部加 latch。

## 尚未实现的 Go parity 能力

当前尚未移植以下 Go 版能力：

```text
能力                          后续方向
----------------------------  ------------------------------------------
mmap node store               持久化 primary/secondary index page
secondary checkpoint index    复用 Go 版 shadow-index flush/recovery 状态机
SST backend                   决策 Parquet 兼容还是 custom SST
minor compaction              immutable WAL -> SST
major compaction              SST merge / garbage cleanup
background dispatcher         使用 Runtime 扩展 background task 能力
manifest append log           替代当前单记录 rename MANIFEST
best-effort WAL repair        当前只实现 strict 和 point-in-time prefix
```

这些能力接入后仍应保持同一 public API 和 Runtime 注入边界。

## 当前性能对比

2026-06-07 使用固定 key/value 生成方式对比 Go 与 C++ WAL/snapshot 主链路。value size 为 64B，Go 版关闭 minor/major compaction 后运行；C++ 版使用 Release 构建。

```text
指标        1K C++ / Go      10K C++ / Go     100K C++ / Go
----------  ---------------  ---------------  ---------------
Put         573 / 790 ns     615 / 748 ns     619 / 753 ns
Get         137 / 206 ns     152 / 233 ns     158 / 226 ns
Scan        86 / 182 ns      87 / 198 ns      81 / 174 ns
SeekGE      211 / 319 ns     221 / 316 ns     234 / 348 ns
```

这组数据只代表当前主链路，不包含后续 mmap node checkpoint、SST 和 compaction 的完整成本。

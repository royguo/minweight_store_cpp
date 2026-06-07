# minweight_store 设计说明

## 当前状态

`minweight_store` 是仓库中的 C++17 KV store 主库，位于 `src/minweight_store/`，生成 public library target：

```text
minweight_store
minweight_store::minweight_store
```

当前实现已经具备 WAL-backed 主链路：

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
manifest                                  未实现
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
4. 追加 WAL delete record 到 mmap segment。
5. 调用 minpatricia::Index::Delete。
6. 释放 primary lock。
7. 返回删除结果。
```

如果 WAL record 已经接受，但 index 更新失败，store 会进入 fatal 状态。这样避免继续在 WAL 与 index 已经分叉的状态下服务请求。

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

当前版本启动时总是从 WAL 重建 heap node index，没有实现 Go 版 primary/secondary mmap checkpoint。因此当前启动成本与 WAL 长度线性相关。后续实现 manifest 与 mmap checkpoint 后，启动路径可以恢复为 Go 版的 checkpoint + tail replay 模型。

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

当前主链路故意不在第一步引入以下能力：

```text
能力                          后续方向
----------------------------  ------------------------------------------
manifest                      记录 checkpoint WAL、active WAL、next file no
mmap node store               持久化 primary/secondary index page
checkpoint                    避免每次 Open 从全量 WAL 重建
SST backend                   决策 Parquet 兼容还是 custom SST
minor compaction              immutable WAL -> SST
major compaction              SST merge / garbage cleanup
background dispatcher         使用 Runtime 扩展 background task 能力
```

这些能力接入后仍应保持同一 public API 和 Runtime 注入边界。

## 当前性能对比

2026-06-07 使用固定 key/value 生成方式对比 Go 与 C++ WAL-backed 主链路。value size 为 64B，Go 版关闭 minor/major compaction 后运行；C++ 版使用 Release 构建。

```text
指标        1K C++ / Go      10K C++ / Go     100K C++ / Go
----------  ---------------  ---------------  ---------------
Put         556 / 790 ns     602 / 748 ns     604 / 753 ns
Get         125 / 206 ns     150 / 233 ns     155 / 226 ns
Scan        81 / 182 ns      82 / 198 ns      77 / 174 ns
SeekGE      202 / 319 ns     211 / 316 ns     221 / 348 ns
```

这组数据只代表当前主链路，不包含后续 manifest、mmap checkpoint、SST 和 compaction 的完整成本。

# minweight_store_cpp

`minweight_store_cpp` 来源于原 Go/golang 版本的 `minweight_store`。本仓库不是继续维护 Go 代码，而是使用 **C++17** 重新构建核心能力，面向后续嵌入 C++/brpc/bthread 代码库的场景。

仓库对外保留两个 CMake target：

```text
Target            Alias                              用途                         现状
----------------  ---------------------------------  ---------------------------  ------------------------------
minpatricia       minpatricia::minpatricia           独立 ordered Patricia index  已完成核心翻译，可单独使用
minweight_store   minweight_store::minweight_store   lightweight ordered KV store WAL + snapshot core 已实现
```

## minpatricia

`minpatricia` 是一个独立的 ordered Patricia index，维护：

```text
byte string key -> opaque Position
```

`Position` 由上层解释，可以指向 WAL record、SST row、page id、外部 record handle 或其他位置。`minpatricia` 只负责有序索引结构，不负责 value 存储、WAL、manifest、fsync、mmap 生命周期或并发控制。

当前状态：

```text
能力                                      状态
----------------------------------------  ----
Get / Put / Delete                        已实现
Probe / Retarget                          已实现
Ascend / Descend                          已实现
range scan / seek                         已实现
heap-backed node store                    已实现
mmap-backed node store                    未实现于当前 C++ 版
独立作为外部 CMake target 使用             支持
```

主要限制：

```text
限制                                      说明
----------------------------------------  ------------------------------------------------
不是线程安全容器                          并发读写必须由上层加锁保护
不存完整 key                              Get 必须通过 RecordStore 取回 key 做精确比较
不存 value                                只保存 key 到 Position 的映射
不提供 crash recovery                     持久化语义由上层 store 决定
内存开销仍随 key 数增长                    不能单独作为海量数据的内存上限控制方案
```

## minweight_store

`minweight_store` 是当前仓库的 KV store 主库，使用 `minpatricia` 作为 primary ordered index。它的目标是提供一个轻量、本地、高性能、有序 KV core，在部分场景下替换更重的 RocksDB 依赖。

当前 C++ 版实现的是 WAL + snapshot checkpoint 主链路：

```text
能力                                      状态
----------------------------------------  ----
Open / Close                              已实现
Put / Get / Delete / Len                  已实现
Scan / ScanRange                          已实现
ReverseScan / ReverseScanRange            已实现
SeekGE / SeekLE                           已实现
mmap WAL append                           已实现
WAL CRC 校验                              已实现
WAL rollover                              已实现
MANIFEST                                  已实现
generation snapshot checkpoint            已实现
旧 WAL generation / snapshot 清理          已实现
Runtime 注入                              已实现
StdRuntime                                已实现
bthread/butex Runtime adapter             未实现
SST / Parquet backend                     未实现
minor / major compaction                  未实现
```

当前文件布局：

```text
db/
  MANIFEST
  SNAPSHOT.<generation>
  wal/
    <generation>/
      <file_no>.wal
```

Crash-safe 语义：

```text
Put/Delete 返回：
  表示写入已经在当前进程内可见。
  不表示该写入已经主动 msync/fsync 到磁盘。

crash recovery:
  默认恢复 WAL 中 CRC 合法的连续前缀。
  遇到第一条非法 record 后截断尾部。
  可以丢失最后一段 suffix 写入。
  不允许跳过中间坏 record 后恢复更后面的写入。
```

主要限制：

```text
限制                                      影响
----------------------------------------  ------------------------------------------------
不是完整 Go 版 parity                     尚缺 mmap node checkpoint、SST、compaction 等能力
不是 RocksDB sync write 语义              Put/Delete 返回不代表强持久化落盘
当前没有 SST/value block 层               大数据集仍不能依靠后台 compaction 控制磁盘/内存形态
checkpoint 会生成 snapshot                当前以 live KV snapshot 简化 Go 版 checkpoint 体系
单 store 写入串行                         写路径由 primary write lock 保护
bthread 适配尚未落地                      Runtime 接口已预留，当前默认实现是 StdRuntime
```

## 文档入口

更详细的设计、任务和迁移记录放在 `docs/`：

```text
docs/agents_read.md
docs/design/minpatricia.md
docs/design/minweight_store.md
docs/trackers/todos.md
docs/worklogs/
```

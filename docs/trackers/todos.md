# TODO

本文档只记录当前未完成任务。任务完成后移动到 `trackers/resolved.md`。

## 当前任务

```text
ID       状态   优先级  任务
-------  -----  ------  ------------------------------------------------------
TODO-2   Open   P0      基于 minpatricia 实现 heap-backed in-memory KV store
TODO-4   Open   P1      移植 Go 版 1MiB manifest append/compact、live SST metadata 和 startup validation
TODO-5   Open   P1      实现 mmap node store 与 primary/secondary checkpoint handling
TODO-7   Open   P2      决策 SST backend：Parquet 兼容、custom SST 或 pluggable backend
TODO-8   Open   P2      移植 minor compaction 和 major compaction
TODO-9   Open   P2      扩展 crash tests、recovery tests、fuzz tests 和 benchmarks 到 Go parity
TODO-10  Open   P1      实现 bthread/butex Runtime adapter 与 blocking I/O worker
```

## 当前执行建议

当前 C++ core 已有单记录 `MANIFEST`、generation snapshot checkpoint 和 WAL generation GC。下一步如果目标是 Go parity，应推进 `TODO-5` / `TODO-7` / `TODO-8`：mmap node checkpoint、SST backend、minor/major compaction。

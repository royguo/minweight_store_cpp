# TODO

本文档只记录当前未完成任务。任务完成后移动到 `trackers/resolved.md`。

## 当前任务

```text
ID       状态   优先级  任务
-------  -----  ------  ------------------------------------------------------
TODO-2   Open   P0      基于 minpatricia 实现 heap-backed in-memory KV store
TODO-4   Open   P1      设计并实现 manifest append、compact 和 startup validation
TODO-5   Open   P1      实现 mmap node store 与 primary/secondary checkpoint handling
TODO-7   Open   P2      决策 SST backend：Parquet 兼容、custom SST 或 pluggable backend
TODO-8   Open   P2      移植 minor compaction 和 major compaction
TODO-9   Open   P2      重建 crash tests、recovery tests、fuzz tests 和 benchmarks
TODO-10  Open   P1      实现 bthread/butex Runtime adapter 与 blocking I/O worker
```

## 当前执行建议

下一步推进 `TODO-4` / `TODO-5`，在已完成的 WAL-backed 主链路上补齐 manifest 与 mmap checkpoint，降低 Open 时全量 WAL replay 成本。

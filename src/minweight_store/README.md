# minweight_store

本目录实现 C++ 版 KV engine，并生成 `minweight_store` library target。

职责边界：

- public store API：`Put`、`Get`、`Delete`、`Len`、`SeekGE`、`SeekLE`、`Scan`、`ScanRange`、`ReverseScan`、`ReverseScanRange`。
- heap-backed in-memory store。
- WAL、manifest、mmap node store。
- primary index 与 secondary checkpoint index。
- flush、recovery、minor compaction、major compaction。
- SST backend 抽象与具体实现。
- `Env`/`Runtime` 抽象、`StdEnv`、后续 `BthreadEnv` 或 I/O offload adapter。

`minweight_store` 依赖并链接 `minpatricia`。读取路径必须始终使用 primary index，checkpoint index 只服务 flush 与 recovery。

公共头文件放在 `include/minweight_store/`，外部用户使用：

```cpp
#include <minweight_store/store.h>
```

CMake target：

```cmake
minweight_store
minweight_store::minweight_store
```

内部实现建议按功能拆到 `internal/`：

```text
src/minweight_store/internal/
|-- compaction/
|-- manifest/
|-- mmap/
|-- record/
|-- runtime/
|-- sst/
`-- wal/
```

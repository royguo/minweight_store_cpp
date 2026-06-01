# runtime

本目录定义和实现运行时适配层。

职责边界：

- `storage_core` 只依赖抽象 `Env`/`Runtime`，不直接绑定 `std::thread`、bthread、Folly、Asio 或某个 coroutine `Task<T>`。
- 首个实现为 `StdEnv`，用于本地开发、单元测试和基准测试。
- 生产 runtime 适配器如 `BthreadEnv` 后续在此目录扩展。

需要抽象的能力包括 mutex、rw mutex、condition/event、semaphore、后台任务、join/cancel、时间、日志、mmap/munmap、msync、fsync、目录 sync、rename/remove/create/open/read/write。

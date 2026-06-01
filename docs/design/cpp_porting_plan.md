# C++ 移植计划

## 结论

C++17/20 移植可行，但应采用结构化移植，而不是逐行翻译 Go 代码。核心目标是保留 ordered index、record position、WAL、manifest、checkpoint、SST install 和 crash recovery 状态机，同时重新设计运行时边界。

## 核心约束

- `minpatricia` 是核心实现，不是可替换细节。
- `minpatricia` 保持同步、CPU-only，不直接依赖 coroutine runtime。
- `storage` 通过注入的 `Env`/`Runtime` 使用锁、后台任务、文件和 mmap 能力。
- 读路径始终使用 primary index。
- crash recovery 的 durability boundary 不能被 coroutine-friendly 改造重排。
- C++ mmap node layout 必须避免 struct layout、alignment 和 aliasing 的未定义行为。

## 阶段计划

```text
阶段  任务                                      产物
----  ----------------------------------------  --------------------------------
1     移植 minpatricia                          C++ index、golden tests
2     实现 heap-backed in-memory store          Put/Get/Delete/Seek/Scan 基线
3     移植 WAL record 与 replay policy          WAL encoding、CRC、replay tests
4     移植 manifest                             append、compact、startup validation
5     移植 mmap node store 与 checkpoint         fixed 4 KiB page layout、sync tests
6     实现 disk-backed store API                Open/Close/Put/Get/Delete/Seek/Scan
7     引入 runtime abstraction 与 StdEnv         blocking-looking core API
8     引入首个生产 runtime adapter              BthreadEnv 或同级 adapter
9     移植 minor compaction                     后台任务与 flush 协调
10    移植 major compaction                     SST merge、garbage policy
11    决策 SST backend                          Parquet 兼容或 custom SST
12    恢复 crash/fuzz/benchmark 覆盖             C++ 验证闭环
```

## 首个里程碑

首个可交付目标是：

- `src/minpatricia` 完成核心 index 行为。
- 用 Go 版本语义生成或手写 golden tests。
- `src/storage` 提供 heap-backed in-memory KV store。
- CMake 能构建并运行相关测试。

## 高风险点

### Scan 语义

Go 版本 scan 期间持有 read lock。如果 C++ 版本允许 async scan callback suspend，可能长期阻塞 writer 或触发 runtime-specific deadlock。

当前建议：

- 第一阶段只支持同步、非 suspend scan callback。
- 后续如需 async scan，采用 bounded batch，在锁内收集 batch，锁外调用用户 async 逻辑。
- snapshot 或 MVCC 作为长期选项，不进入首个里程碑。

### SST 格式

Go 版本使用 Parquet。C++ 直接使用 Arrow Parquet 会引入较重依赖和复杂构建。

待决策选项：

- 保留 Parquet 兼容。
- 先定义 custom SST，再提供 Parquet export/import。
- 抽象 SST backend。

### Crash Recovery

必须保留 WAL append、WAL rollover、primary index sync、active WAL sync、manifest commit、secondary index replay、pending WAL/SST deletion 等顺序约束。

### Mmap Page Layout

固定 4 KiB page 需要显式编码和验证，不能依赖不受控的 C++ struct 内存布局。

必备验证：

- `static_assert` page size。
- 字段 offset 检查。
- endian helper。
- sanitizer。
- crash/reopen tests。

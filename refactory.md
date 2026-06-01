# C++ Refactoring Feasibility Analysis

## Conclusion

Translating this repository to C++17/20 is feasible, but it should not be a
line-by-line rewrite. The right approach is to preserve the core algorithms and
disk-state machine while redesigning the runtime boundary so the C++ version can
work with different coroutine or fiber implementations.

The project depends on `minpatricia`, and that dependency is central to the
store's behavior. `minpatricia` defines the ordered index, node layout, record
position model, and seek/scan behavior. A practical C++ port must include it as
part of the project, not treat it as a replaceable detail.

## Current Go Architecture

The current store is a small single-node ordered KV engine. Its public API is
limited to basic ordered KV operations:

- `Put`
- `Get`
- `Delete`
- `Len`
- `SeekGE`
- `SeekLE`
- `Scan`
- `ScanRange`
- `ReverseScan`
- `ReverseScanRange`

The disk-backed implementation uses:

- a live `minpatricia` primary index for all reads and writes;
- mmap WAL segments under `wal/*.wal`;
- a secondary checkpoint index used by flush and recovery;
- a `MANIFEST` append log for checkpoint and live-SST state;
- Parquet SST files under `sst/*.parquet`;
- background minor and major compaction dispatchers.

The main design property is that reads always use the primary index. Flush and
recovery synchronize checkpoint state without swapping the read path.

## Porting `minpatricia`

Porting `minpatricia` to C++ is feasible and should be done first.

The index is a compact ordered Patricia trie over opaque record positions. It
stores neither keys nor payloads. Keys live in a caller-owned `RecordStore`, and
node pages live behind a `NodeStore`.

Important properties to preserve:

- `Position` is a `uint64_t`-style opaque handle.
- The high bit is reserved as a child-node tag.
- Node pages are fixed-size 4 KiB pages.
- A node can hold up to 339 reps.
- Node IDs are stable and can map directly to mmap page offsets.
- The index supports ordered traversal, lower/upper seek, insert, delete, and
  retargeting.

Recommended C++ representation:

- `uint64_t Position`
- `std::span<const std::byte>` or `std::string_view` for keys
- `Result<T>` or `StatusOr<T>` for error returns
- `NodeStore` and `RecordStore` as concepts or abstract interfaces
- `static_assert(sizeof(NodePage) == 4096)`
- explicit little-endian encoding where data crosses disk boundaries

`minpatricia` itself should remain synchronous and CPU-only. It does not need to
know about coroutines.

## Coroutine-Friendly C++ Design

The C++ version should be structured around a pluggable runtime adapter. The
store core should not directly depend on `std::thread`, `std::mutex`,
`std::condition_variable`, bthread, Folly, Asio, or any one coroutine library.

Recommended split:

### 1. `minpatricia_cpp`

Pure ordered index library:

- no coroutine dependency;
- no file I/O;
- no background tasks;
- no scheduler dependency;
- accepts caller-provided `RecordStore` and `NodeStore`.

### 2. `storage_core`

KV engine implementation:

- WAL;
- manifest;
- mmap node store;
- segmented record store;
- SST layer;
- flush and recovery;
- minor and major compaction;
- store API and concurrency control.

This layer should depend only on an injected `Env` or `Runtime` interface.

### 3. Runtime Adapters

Runtime-specific implementations:

- `StdEnv` using standard C++ threads and locks;
- `BthreadEnv` using bthread mutexes, condition variables, and butex where
  appropriate;
- optional Asio, Folly coroutine, or internal runtime adapters.

This keeps the core engine reusable with different coroutine implementations.

## Required Runtime Abstraction

The key design point is to avoid baking one coroutine return type into the
engine. If the public API directly returns a concrete `Task<T>` type from one
library, the engine is already coupled to that coroutine runtime.

A practical `Env` should abstract at least:

- mutex;
- read-write mutex;
- condition/event notification;
- semaphore;
- background task spawning;
- task joining or cancellation;
- time source;
- logging;
- mmap and munmap;
- msync;
- fsync;
- directory sync;
- file rename/remove/create/open/read/write;
- optional async file operations where the runtime supports them.

For C++17, the simplest coroutine-friendly model is still a blocking-looking
API whose blocking primitives are supplied by the runtime. This works well with
stackful fiber systems such as bthread.

For C++20, an optional async API can be added, but it should be templated on the
runtime:

```cpp
template <class Env>
class Store {
 public:
  Result<void> Put(ByteView key, ByteView value);
  Result<GetResult> Get(ByteView key);

  typename Env::template Task<Result<void>> PutAsync(ByteView key, ByteView value);
  typename Env::template Task<Result<GetResult>> GetAsync(ByteView key);
};
```

The exact type shape can change, but the core rule should remain: the caller's
runtime owns the coroutine type and synchronization primitives.

## Major Design Risks

### Scan Semantics

The current Go implementation holds a read lock while scanning. If the C++
version allows an async scan callback that can suspend, holding the lock across
that suspension is dangerous. It can block writers for a long time or create
runtime-specific deadlocks.

Possible designs:

- keep scan callbacks synchronous and non-suspending;
- expose batch scan, collecting a bounded batch under lock and invoking async
  user code outside the lock;
- add snapshots or MVCC for stable non-blocking iteration.

Snapshots or MVCC would make the engine much closer to RocksDB semantics, but
it is a much larger project.

### Parquet SST

The Go version uses `parquet-go`. In C++, the obvious implementation is Apache
Arrow Parquet, but that brings heavy dependencies and a more complex build.

Options:

- preserve Parquet compatibility using Arrow Parquet;
- define a custom SST format first, then add Parquet export later;
- make the SST backend pluggable.

If file-format compatibility with the Go version is a hard requirement, Parquet
should be treated as part of the first milestone. If not, a custom SST format
will likely reduce project risk.

### Crash Recovery

The current Go code has a careful manifest/WAL/checkpoint/SST install state
machine. The C++ port must preserve ordering around:

- WAL append;
- WAL rollover;
- primary index sync;
- active WAL sync;
- manifest commit with `primary_wal_flushed=true`;
- secondary index replay and sync;
- pending WAL/SST deletion;
- manifest commit with `primary_wal_flushed=false`.

Coroutine-friendly execution must not reorder these durability boundaries.

### Mmap Node Layout

The mmap node store should keep the fixed page model, but C++ must avoid
undefined behavior around struct layout, alignment, and aliasing.

The port should use:

- explicit packed or serialized layout only where safe;
- `static_assert` for page size and field offsets;
- endian conversion helpers;
- sanitizer runs;
- crash/reopen tests on Linux and macOS if both are supported.

### Background Compaction

Minor and major compaction are currently implemented with goroutines and
channels. In C++, they should become runtime-managed background tasks.

The dispatcher should be expressed in terms of the injected `Env`, not hardcoded
threads. This is necessary for bthread/butex compatibility.

## Suggested Implementation Plan

1. Port `minpatricia` to C++ and build golden tests against the Go behavior.
2. Implement heap-backed in-memory store on top of the C++ index.
3. Port WAL record encoding, CRC validation, replay policies, and record
   position encoding.
4. Port manifest encoding, CRC, append/compact logic, and startup validation.
5. Port mmap node store and primary/secondary checkpoint handling.
6. Implement disk-backed `Open`, `Close`, `Put`, `Get`, `Delete`, `Seek`, and
   basic scan.
7. Add runtime abstraction and provide `StdEnv`.
8. Add `BthreadEnv` or the first production coroutine/fiber adapter.
9. Port minor compaction.
10. Port major compaction.
11. Decide whether SST should use Parquet compatibility or a custom format.
12. Rebuild crash tests, recovery tests, fuzz tests, and benchmarks in C++.

## Feasibility Summary

- A C++17/20 implementation is feasible.
- A coroutine-friendly implementation that can work with bthread, butex, or
  other coroutine/fiber runtimes is feasible.
- The implementation should be a structured port with a runtime abstraction,
  not a mechanical rewrite.
- The highest-risk areas are async scan semantics, Parquet dependency choice,
  crash recovery ordering, and mmap page layout correctness.
- The first milestone should be a C++ `minpatricia` port plus an in-memory KV
  store, followed by WAL/manifest persistence and then compaction.

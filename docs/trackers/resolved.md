# Resolved

本文档归档已完成 TODO 和已解决问题。

## 2026-06-01

```text
ID          类型      说明
----------  --------  ---------------------------------------------------------
BOOTSTRAP   Repo      将 Go 版本备份到 golang 分支，并重建 main 为 C++ 初始骨架
```

## 2026-06-05

```text
ID       类型      说明
-------  --------  ------------------------------------------------------------
TODO-1   Code      完成 minpatricia C++17 兼容全功能翻译；覆盖 Go UT/Go fuzz seed/benchmark 项；建立固定 Go-generated key fixture；C++ fixture benchmark 1K/10K/100K 达到或快于 Go 对照
```

## 2026-06-07

```text
ID       类型      说明
-------  --------  ------------------------------------------------------------
TODO-3   Code      完成 minweight_store WAL record encoding、CRC、mmap append、rollover、point-in-time prefix replay 和 strict replay
TODO-6   Code      建立 minweight_store Runtime 注入边界并提供 StdRuntime；主链路锁和 blocking I/O 入口可由 Runtime 替换
STORE-1  Code      实现 C++ 单记录 MANIFEST、generation snapshot checkpoint、WAL generation 文件布局和旧 generation/snapshot GC
STORE-2  Test      增加 Close checkpoint、snapshot+WAL tail replay、WAL prefix recovery 和 strict recovery 测试
BUG-1    Code      修复 Delete 成功路径把 OK Status 隐式构造成 Result<bool>{false} 的返回值问题
```

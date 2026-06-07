# 设计文档索引

本文档是稳定架构与实现设计的入口。开始实现前，应优先阅读本文件和相关设计文档。

## 当前设计

```text
文件                                      状态      说明
----------------------------------------  --------  ------------------------------
design/cpp_repository_layout.md           Active    `minpatricia` 与 `minweight_store` 双产物目录、CMake 目标与测试布局
design/cpp_porting_plan.md                Active    Go 到 C++ 的分阶段移植计划
design/minpatricia.md                     Active    `minpatricia` 模块职责、数据结构、算法原理、API 使用和性能边界
design/minweight_store.md                 Active    `minweight_store` WAL-backed 主链路、Runtime 注入和恢复语义
design/minpatricia_port_plan.md           Active    `minpatricia` C++ 翻译计划、正确性测试和性能基线
design/minpatricia_go_ut_coverage.md      Active    Go 版 `minpatricia` UT/benchmark 到 C++ 测试与 benchmark 的覆盖矩阵
```

## 更新规则

- 新模块、大规模重构、状态机、恢复流程、并发一致性路径，必须在实现前或实现同步更新设计文档。
- 未定案的方案先放入 `discuss/`，形成结论后再同步到 `design/`。
- 每次 commit 前检查本索引是否需要更新。

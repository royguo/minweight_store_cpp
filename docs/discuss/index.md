# 讨论文档索引

当前没有打开的讨论文档。

后续需要先讨论、再定案的问题：

```text
主题                    状态   说明
----------------------  -----  --------------------------------
SST backend 选择         Open   Parquet 兼容、custom SST 或 pluggable backend
Async scan 语义          Open   suspend callback、batch scan、snapshot/MVCC 的边界
BthreadEnv 适配形态      Open   mutex、condition、butex、task lifecycle 的接口形状
```

讨论形成可执行结论后，应同步更新 `trackers/todos.md` 和相关 `design/` 文档。

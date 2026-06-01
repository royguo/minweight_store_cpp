# minpatricia

本目录实现仓库内置的 C++ `minpatricia`。

职责边界：

- 只实现同步、CPU-only 的 ordered Patricia trie。
- 不直接依赖文件 I/O、mmap、后台任务或 coroutine runtime。
- 通过调用方提供的 `RecordStore` 和 `NodeStore` 访问 key 与 node page。
- 保留 Go 版本的核心语义：`Position` 为 `uint64_t` 风格 opaque handle，高位作为 child-node tag，node page 固定 4 KiB，节点 ID 可稳定映射到 page offset。

测试与源码同目录放置。示例布局：

```text
src/minpatricia/
|-- patricia_trie.cc
|-- patricia_trie.h
|-- patricia_trie_test.cc
|-- node_page.cc
|-- node_page.h
`-- node_page_test.cc
```

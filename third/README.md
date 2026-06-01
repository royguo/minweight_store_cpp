# third

第三方依赖统一放在本目录。

规则：

- 优先使用源码内置、submodule 或明确版本的 vendored dependency，避免隐式依赖系统环境。
- 新增依赖前需要在 `docs/discuss/` 记录取舍，稳定后同步到 `docs/design/`。
- `minpatricia` 不放在 `third/`，它是本仓库核心实现的一部分，位于 `src/minpatricia/`。

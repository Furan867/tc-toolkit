# simplexc CLI 与游戏壳编译管线分叉

- 状态：未修
- 影响：hotspot 的离线编译（`hotspot/run.py` 用 `simplexc_cli`）与游戏内编译（`simplexc_compile`）；任何只在其中一侧加入的 pass、选项或源码改写。
- 现象：同一份 `.si` 可能被两侧编成不同 blob；hotspot 用 CLI 产物去 `locate` 游戏内副本时找不到，采样直接失败。
- 判据：同一份源、同一组参数，CLI 与游戏壳产物 sha256 一致；新增优化只接在共享核心后，两侧仍一致。
- 已确认：
  - CLI 在 `simplexc/cli/simplexc_cli.cc:522-609` 自己接 `Frontend → build_module → analyze → Program → assemble`。
  - 游戏壳在 `simplexc/shell/simplexc_core.cc:197-219` 另接一遍同样的核心模块。
  - 2026-11-07 的 guarded-div 实验只改 shell 的 `simplexc_compile()`：CLI 完全不受 `TC_GUARDED_DIV` 影响，编出基线 blob（`c14e23…`，入口 `0x5300`），游戏内是 guarded blob（`eb92e4…`，入口 `0x5310`），hotspot `locate` 失败。
  - 普通参数下两侧目前可以逐字节一致（此前基线比对过）；但这是“两套胶水恰好同参”，不是结构保证。
- 已排除：
  - 不是 hotspot `run.py` 的路径或缓存问题；换成同源基线/已 guard 源即可对齐。
  - 不是 `python -m services.simctl`；那是架构 §5.5 为避免 in-process 副作用与保留测试 mock 而有意保留的。
- 下一步：把“源 → 产物”抽成一条共享编译函数/管线，CLI 与 `simplexc_compile()` 都调用；CLI 的额外输出（obj、debug info、elements、offsets）作为参数保留，不再复制 glue。
- 复现：
  1. 在一侧入口加入任意只影响产物的 pass（例：只在 `simplexc_compile()` 改写）。
  2. 用 `simplexc_cli` 编译同一份 `.si`，比较两侧 blob sha。
  3. 或直接跑 `hotspot/run.py`：CLI 产物与游戏内副本 sha 不同，报 `没找到产物副本`。

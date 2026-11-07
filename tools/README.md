# tools/

用户敲的命令。**这里不放库**——被复用的实现去 `libs/` 或 `services/`。

| 位置 | 是什么 |
|---|---|
| `perf_opt/` | 转发层的用户入口（构建 / 部署 / 配置 / 编译后优化） |
| `freq/` | 主频测量包（`watch_freq` / `read_freq` / `bench_rewrite` 的实现） |
| `decompile/` | 逆向脚本（零产品依赖，不参与依赖规则） |
| `watch_freq.py` | 产品入口：被动监听主频 |
| `read_freq.py` | 服务壳：读一次当前主频（hotspot 也从命令行调它） |
| `deploy_compile_hook.py` | 薄壳：转发到 `perf_opt/cli.py`，命令与以前完全一样 |
| `bench_rewrite.py` / `check_shift_equiv.py` | 实验脚本，不是产品 |
| `try_compile_dll.py` | 离线加载 `compile.dll` 调 `compile()`（`simplexc/tools/` 与 `perf_opt` 用它） |

**新增工具的判据**：它会不会被别的工具 import？会 → 实现放 `libs/` 或 `services/`，这里只留入口。

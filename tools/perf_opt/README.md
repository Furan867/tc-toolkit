# tools/perf_opt/

转发层的**用户入口**：构建、部署、配置、编译后优化。入口是 `cli.py`（`python tools/perf_opt/cli.py build-deploy`）。

| 模块 | 职责 |
|---|---|
| `cli.py` | 五个子命令：`status` / `build` / `deploy` / `build-deploy` / `restore` |
| `build.py` | 转调 `simplexc/tools/build.py`（构建定义只有 CMakeLists 一处） |
| `deploy.py` | 拷贝 dll、备份/还原游戏目录的 `compile.dll`、状态查询 |
| `config.py` | `opt.ini` 的模板与部署侧读写（**不改** `tools/freq/` 那一半） |
| `rewrite.py` | 编译后优化的**唯一入口**（C 壳的 `OPT_SCRIPT` 指向它） |
| `optimizer/` | 改写器本体：`core.py`（解析/规则/重定位）、`analyze.py`（冗余统计） |
| `format_si.py` | 离线格式化捕获的 `.si`（**编译路径不调用**：捕获产物必须保真） |

**依赖规则**：

- `optimizer/*` 只 import 标准库与 `libs.hook_log`；**不许** import `tools.*`，也不许回头 import `rewrite`/`cli`。
- 编译后优化是**可删单元**：移走 `rewrite.py` + `optimizer/` + 去掉 `native/compile/processor_rewrite.c` 的注册，编译仍能过。

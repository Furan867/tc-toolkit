# native/

C 侧代码。**三个子目录，依赖方向单向**：

| 目录 | 是什么 | 依赖 |
|---|---|---|
| `forward/` | 转发**机制**：在自身模块旁找原实现、调用编号、捕获落盘、追加日志、SHA-256、读配置、加载时清理 | 只有 Windows API |
| `compile/` | compile **服务**：Result 形状、锚点、manifest、处理器注册表、`DllMain`、导出函数 | `forward` |
| `backend/` | **实现**：`simplexc.c`（进程内调 C++ 核心）、`ref.c`（转交真实 dll） | `forward` + `backend.h` |

**规则**（违反就是分层破了）：

- `forward/` 里不许出现 `compile` / `simplexc` / `opt.ini` / `work/` —— 路径与配置全由壳经 `forward_init` 注入。
- `simplexc_core.h` 只许被 `backend/simplexc.c` include。
- 一个 dll 只有一个 `DllMain`，它在 `compile/`。
- 构建定义在 `simplexc/CMakeLists.txt`（要链 C++ 核心与 LLVM-C）；`native_forward` 是 `STATIC`，将来每个转发壳各链一份。

将来要转发别的 dll（例如 `game_engine`）时：**加一个壳目录**（如 `native/game_engine/`），链 `native_forward`，业务差异只写在那个壳里。

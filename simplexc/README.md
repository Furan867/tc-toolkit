# simplexc

把 Simplex 电路源码编成游戏能 JIT 的 blob（自研替换 `compile.dll`，目标是提升游戏内仿真主频）。
正确性判据：同一份 `.si`，我们的产物与参考产物**逐 tick 状态等价**。

## 目录职责

| 目录 | 负责什么 | 现状 |
|---|---|---|
| `frontend/` | 源码文本 → 类型化 AST | **未开始** |
| `lower/` | 类型化 AST → LLVM IR | **未开始** |
| `runtime/` | 路线 B 运行时（编成 COFF，随 blob 装配） | 只有 `rt_min.cc`（spike 用） |
| `assemble/` | COFF → blob | 可用 |
| `shell/` | 壳里新增的那部分 | 只有入口桩与链接探针；选实现/失败转交仍在 `tools/compile_hook.c` |
| `cli/` | 离线 CLI（不碰游戏） | `tcblob` / `llvm_prog` / `prog_empty` |
| `tools/` | 语料/判据/宿主脚本 | 见下「判据」 |
| `corpus/` | 98 份语料 + manifest（`ref/` 不进 git） | 已固化 |
| `dossier/` | 语义档案（接口 §9 的实验落地处） | 未开始 |

每个目录一个 README（负责什么 / 现在有什么 / 对外接口在哪 / 怎么用）。

## 构建

    python simplexc/tools/build.py [all|objs|tcblob|llvm_prog|hook|spike]    # 默认 all

需要 w64devkit（默认 `D:/bin/w64devkit/bin`，可用 `W64DEVKIT_BIN` 覆盖）与 `build/llvm/`
（头 + 自造 MinGW 导入库，不进 git；没有就先 `python tools/fetch_llvm.py --dest D:/downloads`）。
产物全部落在 `reversing/build/`，两条路径写法上的硬要求见 `simplexc/tools/build.py` 头部。

## 判据

| 判据 | 命令 |
|---|---|
| M0 语料/manifest 一致 | `python simplexc/tools/recompile_corpus.py --check` |
| M2 差分宿主自比对 | `python simplexc/tools/host/selftest.py --src simplexc/corpus/src/006.si --bin simplexc/corpus/ref/006.bin --ticks 3` |
| 产物能在进程内跑一遍 | `build/tcblob.exe --obj … --entry __tc_entry --out x.bin --run` |

主设计 §7.5 那条「一条命令跑完全语料差分」还没实现；游戏内部分一律 manual/slow。

## 接口与设计

- **接口冻结版 1**：`docs/specs/2026-10-15-simplexc-interfaces.md`（改它 = 改契约，要登记版本号）
- 主设计：`docs/specs/2026-10-15-simplex-llvm-compiler-design.md`
- 当前里程碑：M0 语料 / M1 spike / M2 差分宿主已完成，前端 + Lowering 未开始 —— 详见 `docs/status.md`

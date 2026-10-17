# simplexc

把 Simplex 电路源码编成游戏能 JIT 的 blob（自研替换 `compile.dll`，目标是提升游戏内仿真主频）。
正确性判据：同一份 `.si`，我们的产物与参考产物**逐 tick 状态等价**。

## 目录职责

| 目录 | 负责什么 | 现状 |
|---|---|---|
| `frontend/` | 源码文本 → 类型化 AST（**C++17**，接口 `frontend/ast.h`） | C++ 版未开始；现有 `.py` 是**冻结的原型** |
| `lower/` | 类型化 AST → LLVM IR（**C++17**） | 同上 |
| `runtime/` | 路线 B 运行时（编成 COFF，随 blob 装配） | 只有 `rt_min.cc`（spike 用） |
| `assemble/` | COFF → blob | 可用 |
| `shell/` | 壳里新增的那部分 | 只有入口桩与链接探针；选实现/失败转交仍在 `tools/compile_hook.c` |
| `cli/` | 离线 CLI（不碰游戏） | `tcblob` / `llvm_prog` / `prog_empty` |
| `tools/` | 语料/判据/宿主脚本 | 见下「判据」 |
| `corpus/` | 98 份语料 + manifest（`ref/` 不进 git） | 已固化 |
| `dossier/` | 语义档案（接口 §9 的实验落地处） | 未开始 |

每个目录一个 README（负责什么 / 现在有什么 / 对外接口在哪 / 怎么用）。

**产品 vs 原型**：产品路径（进 DLL）一律 **C++17** —— `frontend/` / `lower/` / `runtime/` / `assemble/` /
`shell/` / `cli/`；`tools/` 下的 Python 只做离线工具。`frontend/*.py` + `lower/ir_build.py` 是 M3 的
**冻结原型（oracle）**：默认只读，不入产品路径，禁止在其上开发，也禁止「先 Python 后移植」
（权威清单 = 接口冻结版 §0；理由与禁令 = 主设计 §3.1、§8）。

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
- 当前里程碑：M0 语料 / M1 spike / M2 差分宿主 / M3（宿主面，Python 原型）已完成；
  前端 + Lowering 的 **C++ 产品版未开始（M4.5，M5 之前完成）** —— 详见 `docs/status.md`

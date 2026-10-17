# lower

类型化 AST → **LLVM IR**：语义保真映射（内存模型、`$` 写回、extern 声明、entry 包装）、内建映射表，
以及固定版本的 pass pipeline（`LLVMRunPasses` 的 `-O2`）。

**现状**：C++17 版未开始；目录里已有 Python 原型 `ir_build.py`（M3 的产出）。
**产品实现 = C++17（待写，M4.5）**；`ir_build.py` 是 M3 的**冻结原型（oracle）**：默认只读、禁止在其上开发
—— 见接口冻结版 §0、主设计 §3.1/§8。接口两侧都已冻结：输入见接口 §1，输出侧的
三元组 / ABI / pass 见接口 §7 与主设计 §4。

- 对外接口（冻结版 1）：`docs/specs/2026-10-15-simplexc-interfaces.md` §1（输入）、§7（链接与目标）
- 语义保真的验收面：主设计 `docs/specs/2026-10-15-simplex-llvm-compiler-design.md` §5；
  §5.3 说明了**为什么禁止转译**
- 目标形状：`x86_64-pc-windows-gnu`、`RelocMode=Static`、`CodeModel=Small`、禁 uwtable/异常
- **禁止猜的语义**：接口 §9（`load`/`store` 的地址单位等是地基）

能跑的样板：`simplexc/cli/llvm_prog.cc`（LLVM-C 生成 `tc_main` → COFF → 装配器）、
`simplexc/shell/llvm_link_probe.cc`（最小链接探针）。

怎么用：还没有可跑的东西。接上后先走 `cli/` 的离线路径，别直接进游戏。

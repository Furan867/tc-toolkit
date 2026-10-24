# codegen

**HIR → LLVM IR**（主设计 §3.2 的终态）：语义保真映射（内存模型、`$` 写回、extern 声明、entry 包装）、
内建映射表，以及固定版本的 pass pipeline（`LLVMRunPasses` 的 `-O2`）。输入只有 `sema::Hir`
（`sema/` 已于 S2 拆出，本目录原先叫 `lower/`）。

**现状**：**C++17 实现已落地并接上产品路径**（`49568d8`；旧的无类型 AST → IR 那条路已删）。
`module_ast.hh` / `module_source.hh` / `module_build.{hh,cc}` 是还没消掉的过渡物 —— 它们把前端产出的
`ModuleAst` 搬成 `sema::ModuleSource`，与旧 lowering 无关。M3 期的 Python 原型 `ir_build.py` 已于 2026-10-18 删除
（git 历史可查），不再有任何实现被当作语义权威 —— 语义以原版编译器的实测行为为准。
接口两侧都已冻结：输入见接口 §1，输出侧的
三元组 / ABI / pass 见接口 §7 与主设计 §4。

- 对外接口（冻结版 1）：`docs/specs/2026-10-15-simplexc-interfaces.md` §1（输入）、§7（链接与目标）
- 语义保真的验收面：主设计 `docs/specs/2026-10-15-simplex-llvm-compiler-design.md` §5；
  §5.3 说明了**为什么禁止转译**
- 目标形状：`x86_64-pc-windows-gnu`、`RelocMode=Static`、`CodeModel=Small`、禁 uwtable/异常
- **禁止猜的语义**：接口 §9（`load`/`store` 的地址单位等是地基）

能跑的样板：`simplexc/cli/llvm_prog.cc`（LLVM-C 生成 `tc_main` → COFF → 装配器）、
`simplexc/shell/llvm_link_probe.cc`（最小链接探针）。

怎么用：`python simplexc/tools/check.py`（判据入口）；离线路径走 `cli/`，别直接进游戏。

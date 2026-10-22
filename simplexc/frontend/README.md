# frontend

源码文本（`.si`）→ **类型化 AST**：词法/语法、`#if in_scope` 与预处理、作用域、类型系统、
常量求值、重载解析、内建识别、根确定与 DCE。

**现状**：**C++17 实现已落地**（M4.5），接口 `frontend/ast.h`、`frontend/frontend.h`。
M3 期的 Python 原型（`decls.py` / `expr.py`）已于 2026-10-18 删除（git 历史可查），
**不再有任何实现被当作语义权威** —— 语义以原版编译器的实测行为为准（`docs/facts/semantics.md`）。

判据入口：`python simplexc/tools/check.py`；构建：`python simplexc/tools/build.py`。

结论与语法摸底：`docs/notes/2026-10-18-frontend-decls.md`（顶层构造只有五种、`#if` 与
标签块怎么处理、可达性口径与 dossier 里 19 的差异）。

结论与语法摸底：`docs/notes/2026-10-18-frontend-decls.md`（顶层构造只有五种、`#if` 与
标签块怎么处理、可达性口径与 dossier 里 19 的差异）。

- 对外接口（冻结版 1）：`docs/specs/2026-10-15-simplexc-interfaces.md` **§1**（类型化 AST 与类型表示）、
  **§2**（内建表 schema，唯一来源）
- 必须交给 LLVM 什么：主设计 `docs/specs/2026-10-15-simplex-llvm-compiler-design.md` §5
- 组件职责表：主设计 §3.1
- **禁止猜的语义**：接口文档 §9 的 U-L1…U-L14 —— 先做 `dossier/` 的实验，再把结论写死

类型表示、泛型单态化、内建表与可见性规则都已经冻结，不要按自己的理解先写一版。

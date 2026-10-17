# frontend

源码文本（`.si`）→ **类型化 AST**：词法/语法、`#if in_scope` 与预处理、作用域、类型系统、
常量求值、重载解析、内建识别、根确定与 DCE。

**现状：只有第一步**（`decls.py`：词法掩码 + 顶层声明表 + 花括号配平 + 名字级可达性）。
表达式/语句/类型一律还没有：函数体**只配平不解析**（纪律见 `docs/status.md`「下一步」第 2 条）。
类型表示、泛型单态化、内建表与可见性规则都已经冻结，不要按自己的理解先写一版。

```
python simplexc/frontend/decls.py --src simplexc/corpus/rv32im/rv32im.si   # 声明表(TSV) + stderr 统计
python simplexc/frontend/decls.py --src <file> --json out.json              # 同上，结构化
```

结论与语法摸底：`docs/notes/2026-10-18-frontend-decls.md`（顶层构造只有五种、`#if` 与
标签块怎么处理、可达性口径与 dossier 里 19 的差异）。

- 对外接口（冻结版 1）：`docs/specs/2026-10-15-simplexc-interfaces.md` **§1**（类型化 AST 与类型表示）、
  **§2**（内建表 schema，唯一来源）
- 必须交给 LLVM 什么：主设计 `docs/specs/2026-10-15-simplex-llvm-compiler-design.md` §5
- 组件职责表：主设计 §3.1
- **禁止猜的语义**：接口文档 §9 的 U-L1…U-L14 —— 先做 `dossier/` 的实验，再把结论写死

怎么用：`decls.py` 先给出「哪些声明占哪几行、哪个 def 可达」的骨架，后续语句/表达式解析按
`--json` 里的 `lo`/`hi` 切片；与 `lower/`、`cli/` 共用核心，产物先接离线 CLI，再接壳。

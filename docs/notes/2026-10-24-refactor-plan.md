# 重构方案（M5.5）：lower/ → sema/ + codegen/

状态：方案定稿，待执行。负责人 2026-10-24 确认「按 A（分两步），S1 可以弱，终态必须满足设计文件」。
上游：`docs/specs/2026-10-15-simplex-llvm-compiler-design.md` §3.2–§3.4 / §7.6，
`docs/specs/2026-10-15-simplexc-interfaces.md` §1b（HIR 目标形态）。

## 0. 事实底数（实测，2026-10-24）

- `lower.cc` 3428 行；`Lower`/`Program` 成员函数 144 个 2738 行：纯 sema 31/519、纯 codegen 54/509、
  **mixed 59/1710（62.4%）**；另自由函数 31 个 409 行、`module_build.cc` 整文件 0 LLVM。
- 机械拆分上限 = 19 成员(330) + 31 自由(409) = **739 行 ≈ 21.6%**；其余都触碰
  `V{ll,ty,c}` / `Binding{ptr,…}` / `FnState` / `Closure` / `Capture{ptr,…}` / `Fn`。
- 逐字节回归判据已可执行：98 份全量 **8.5s**，比的是掩掉 `stub/slot` 槽位的 `norm_sha256`；
  raw sha 只在同一次开机内跨进程可比（跨 boot 会变，别当判据）。
  `python simplexc/tools/blob_baseline.py --verify …`。

## 1. 终态（不可打折的部分）

| 要求 | 出处 | 验收 |
|---|---|---|
| sema 不发任何 LLVM 指令；codegen 不查 AST/源码、不做类型推导、不决定实例化 | §3.2 | `grep -c LLVM simplexc/sema/` == 0；codegen 的 include 里没有 `frontend/ast.hh` |
| HIR 六条不变量 | §3.2 | 每条一个 `assert` + 一条最小用例 |
| 无 save/restore 可变状态；递归用子上下文或显式参数 | §3.3 | `grep -c 'save_state\|restore_state'` == 0 |
| 错误三轴从第一层带出 | §3.4 | 失败用例表：责任/阶段/span 可区分 |
| 实例键唯一且规范化，实例集合由 sema 定死 | §1b / §3.2-6 | 同 (def, 实参, 环境) 只出现一次，dump 可见 |

## 2. 关键决策（本方案定的，写进代码注释，不进 specs）

1. **HIR = AST 的去糖投影 + 显式 cast + 已解析引用**，独立节点树（`sema/hir.hh`），不复用 AST 节点。
   理由：不变量 4（隐式转换显式化）需要插节点；codegen 不许看源码，而 AST 带语法糖
   （`#if`、`const #`、`let/var` 区别、体内 `def`）。**不做三地址码/MIR** —— LLVM IR 就是 MIR（§3.2）。
2. **codegen 保留自己的值表示与作用域表**（`V{ll,ty}` 照旧）。§3.3 只说「sema 的 V 不带 ll」，
   没要求 codegen 改值表示。分层后 codegen 的 `V.ty` 从 HIR 读、不从 AST 推。
3. **局部引用用名字**（不引入 slot 编号），让 alloca 顺序/命名不变 → 逐字节判据能兜住。
4. **常量折叠全部留在 sema**：HIR 的 `Const` 节点已折叠（含截断，语义见 `facts/semantics.md`）。
   codegen 见到 `Const` 直接用，不再有 fold 分支。
5. **`#if in_scope` 判定与删子树在 sema**（§3.2 审查更正）；前端只留 `PpIf` 节点。
6. **内建在 sema 解析成 `BuiltinId`**（带 mutating/返回/对齐语义），HIR 里不留源码名字。
7. **实例键** = `<函数名>$<实参规范化串>`，串由类型规范化函数生成（递归：bits/signed/struct 名/array elem）；
   闭包面另带捕获签名（现状键的第 4 位）。规范化函数**唯一一处**，sema 与 codegen 都调它。
8. **业务不双跑**：不保留「旧 lowering 路径 + 新 HIR 路径」并存。迁移点放在最后一次提交，
   中间提交保持 check.py 全绿（新增 `sema/` 与 dump 工具不接线）。

## 2b. HIR 规格（冻结，落码 `simplexc/sema/hir.hh`）

节点集 = AST 的去糖投影。**不含**语法糖节点（`#if`、`const #`、`let/var` 区别、体内 `def` 声明、`if_case` 容器）。

| 类别 | 节点 | 载荷 | 备注 |
| --- | --- | --- | --- |
| 表达式 | `Const` | 位模式 u64 | 已折叠；`ty` 已定宽 |
| | `StrLit` | 串池 id | 字节与 cstring 标志在 `Hir.strings` |
| | `Local` | 名字 | 已绑定到局部声明 |
| | `Global` | 全局 id | |
| | `Capture` | 序号 | 闭包捕获 |
| | `Unary` / `Binary` | op | 结果类型已定（`arith_ty`/`unify` 在 sema 跑完） |
| | `Cond` | 3 子 | |
| | `Call` | callee = `FnId` 或 `BuiltinId` | 不许留源码名字 |
| | `Member` | 字段序号 + 偏移 | 布局由 sema 定 |
| | `Index` | 2 子 | |
| | `Cast` | `{Trunc, Sext, Zext, PtrToInt, IntToPtr, Raw}` | **隐式转换一律走这里** |
| | `ArrayLit` / `CompositeLit` | 子序列 | 字段初值按声明序 + 偏移 |
| | `Template` | literal / expr 交替 | `Str` 插值 |
| 语句 | `Block` `VarDecl` `Assign` `ExprStmt` `If` `While` `Switch` `Break` `Return` | | `Assign` 的 lhs 已是 HIR 左值（`Local`/`Member`/`Index`） |

顶层：

```text
Hir { HirFunc main; funcs[]; globals[]; externs[]; strings[]; consts[] }
HirFunc { key(规范化实例键); name(产物符号名); params[]; caps[]{name,ty,index}; body; ret }
```

**六条不变量，每条一个 `assert`**：I1 每节点 `ty` 非空（除 Void 语句）I2 `Local`/`Global`/`Capture` 指向已解析声明
I3 `Call` 的 callee 一定已在 `funcs` 里（实例闭包已收敛）I4 无隐式转换（宽度/符号变化必经 `Cast`）
I5 每节点带 span I6 `funcs[].key` 唯一。

**接缝判据**：`hirdump` 把 HIR 打成前缀式一行一节点，可直接 diff；sema 单独喂 AST 即可产出它，不碰 LLVM。

## 3. 分步与提交边界

| 步 | 内容 | 判据 | 提交 |
| --- | --- | --- | --- |
| **S1** | 搬 `lower.cc` 的 31 个自由函数 + `module_build.cc` 到 `sema/`（不动类定义、不改一行逻辑） | `blob_baseline --verify` 98/98 一致 + check.py 全 PASS | 1 个 |
| **S1b** | 搬类型系统 + 常量折叠到 `sema/types.hh`；搬 19 个干净成员函数；`class Lower` 按语义侧/LLVM 侧拆成员 | 同上 | 1 个 |
| **S2a** | 新增 `sema/hir.hh` + HIR dump（`hirdump`，不进产品），不接线 | 编译过、check.py 全 PASS、dump 能出 98 份 | 1 个 |
| **S2b** | sema 生成 HIR：类型/常量/绑定/实例闭包（sema 侧完整） | 最小用例 + dump 抽样核对；不接线 | 1–2 个 |
| **S2c** | codegen 改吃 HIR（一次性切换），删 `static_type`/`cv`/`unify` 等推导 | 98/98 逐字节一致 + check.py 全 PASS | 1 个 |
| **S2d** | 拆 `V`/`Binding`/`Capture`/`FnState`：消 save/restore，递归改子上下文 | 同上 + `grep save_state` == 0 | 1 个 |
| **S3** | 错误三轴 + 消除 §3.4 的 6 条静默降级 | 失败用例表 + 回退率按三轴 | 1–2 个 |
| **S4** | §7.6：最小用例、assert、接缝；重跑 M5 判据重记账 | check.py 含新步骤 | 1–2 个 |

S1 与 S1b 是「纯搬」，产物必须逐字节相同；S2c 起也要求逐字节相同（纯语义保持），
唯一允许改变产物的是 S3（错误路径）与 S4 之后的优化。

## 4. 写者分区（并行度）

- 同一时刻：`sema/` 一个写者、`codegen/`（原 `lower/`）一个写者、`tools/` 一个写者。
- S2b（sema）与 S2c（codegen）可以在 `hir.hh` 冻结后**并行**，前提是 hir.hh 只由主程改。
- `docs/` 只由主程写；子 agent 的过程写 `docs/notes/<agent>-<topic>.md`。

## 4b. §3.4 勘误与两个决策（依据 `work/tmp/silent/paths.md`，HEAD=b5d90c9）

设计 §3.4 列的六条静默路径，两处引文不准、一处定性错。specs 只读，所以勘误记这里，S3 执行时按此办：

| 条 | 设计原文 | 实测 |
| --- | --- | --- |
| P5 | `parser.cc:292-311`「顶层未知行被解析成合法表达式」 | 引错位置：那是 `parse_block`。顶层未知行在声明扫描 `decls.cc:479-484` 就被丢进只写不读的 `top_unsupported`，**从未进解析器**；rc=0 照出 blob 是真的 |
| P6 | 「不可达 def 里的语法错不报」列为静默降级 | **定性错**：原版编译器同样不报（`bad.si` 死代码多一个 `)` → rc=0、8581 B）。判定点是 `module_build.cc:165` 的唯一闸门。反向：不可达 def 里的**词法**错照样拦（rc=1） |
| P1 | `parser.cc:344` | 兜底在 341-352，344 是中间行 |
| P4 | 「对 struct 字段只 catch 并继续」 | 位置对（3313-3329），但错在**位置**：它把错误记进 `record`（518-521）而不是抛 `Unsupported`，所以后续流程当它「已处理」 |

**D1（P6 怎么处理）：显式记录，不改行为。** 差分是唯一正确性判据（§7.1），「不可达 def 里不报」是与参考一致的保真行为，不是缺陷。
所以「禁止静默降级」= 不能**悄悄**，不等于必须变行为：记为 `responsibility=用户源码错 / stage=语法 / span 有` 的可查条目，行为照旧。

**D2（P2 的 notes）：先给 warnings 加消费者（sidecar + hook.log），不升级成错误。** 只有当某条 note 表示「我们跳过了某个语义」时才升成 `UnsupportedConstruct` —— 那才叫降级。

## 5. 已知风险

- S2c 一次性切换若产物变化，定位靠三件：`ir_text()` 逐函数 diff、单份语料 CLI 重编、git bisect 到块级。
- `ensure_def_fn`（75 行，换侧 10 次）是「发函数体时又发现新实例」的按需生成；分层后必须改成
  sema 先算闭包。它是 S2b 的最难点，先做它再谈其他。
- 传 `Ty` 的 `is_bool` 名义位不能丢（插值渲染 `true`/`false` vs `1`/`0`），否则 S2 会静默改行为。

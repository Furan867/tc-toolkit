# 嵌套 def 互调 / 捕获摊平（`lower/**`）+ 程序根体 `var` 口径的实验结论

日期 2026-10-18 · 改 `simplexc/lower/{lower.hh,lower.cc}` · 现场 `work/tmp/closure/`
规格来源 = `docs/notes/2026-10-18-in-scope-fix.md` §6/§7（口径问题由本轮的片段实验定）

## 0. 结论先行

1. **捕获按「最外层调用点」摊平**（实现已落）：调用一个被调嵌套 def 时，把它自身**和它按名调用的
   兄弟/外部嵌套 def**的捕获一并算在**本函数的调用点**上、再原样往下传。006/058 的 `halt`、
   110 的 `seen` 都是这个形状。
2. **程序根体的 `var` 不是全局**（实验定，未实现）：自由变量按**调用点链**解析 —— 与 def 的文本
   位置无关；链条要走到「最外层调用点」才可能命中。我们把这层链条断在了**顶层 `def` 的边界**上，
   所以 8 份仍停在 `未定义的名字`。见 §3（要机制改动，已上报主程定）。
3. 98 份口径：**3 全等 + 95 cli_fail** → **16 可装配 · 15 全等 · 1 diff · 82 cli_fail**。
   7 份「假 equal」全部回到**真 equal**（10 tick 差异 0 条）。

## 1. 实现：捕获的传递摊平（`lower.cc` 的 `captures_of`）

`captures_of()` 撞到 `Binding::Kind::Def`（兄弟嵌套 def）原来是 `throw Unsupported`。改成：
把那个引用的 def 收进 `Closure::defs`、并**递归摊平它的捕获**（`collect_captures`，带 `visiting`
集合防互调环）；`ensure_nested_fn` 先绑 `defs` 里的名字（`Binding::nested`）再绑参数/捕获，
调用点把 `cl.caps` 全部作为指针实参传下去。

为什么只能这样：参考实现按**调用点**展开被调的嵌套 def（`in-scope-fix.md` §6 第 1 条），而我们把它
生成成**真函数** —— 兄弟 def 的自由变量只能由调用它的那一层带进来。lookup 找不到 Def 时也查
`root_defs_`（程序根语句树里的嵌套 def 不在词法作用域里，`call_named` 走的就是那张表）。

## 2. 判据（原始输出）

```text
python simplexc/tools/m5/coverage.py
  ── 总 98 · 可装配 16 · 逐 tick 全等 15 · cli_fail 82 · diff 1 · 装配回退率 83.7%

全等的 15 份（`work/tmp/m5/coverage.tsv` 的 status=equal，diff_ticks=0）：
  002 004 005 006 020 026 030 036 038 058 071 120 176 177 180   + baseline/rv32im

python simplexc/tools/check.py   → 5 PASS（lower_equiv：10/10 tick · diff_count=0）
pi-lens（ast-grep 同规则集）     → 新增代码 0 条（probe.cc 同命令报 2 条，证明入口有效）
```

上一轮的 3 份（176/177/180）**没有一份变 diff**；新出现的 176/180 之外的全等份（005/030/036/071/120）
也都在。026/038/058 从 cli_fail 直接变 equal。

## 3. 程序根体 `var` 口径：实验定的是「调用点链」，不是全局（**未实现**）

器材：`python work/tmp/ul14/refcc.py <片段> <out.bin> --markers …`（离线参考编译器 + 标记命中表）；
片段在 `work/tmp/closure/`。

| 片段 | 形状 | 参考实现 | 说明了什么 |
| --- | --- | --- | --- |
| `rootvar_E_def_then_var.si`（上一轮） | root 体：def（引用 x）→ `var x` → 调用 | 编过 | 解析**不在** def 的文本位置（那里 x 还没声明） |
| `rootvar_K_guard_before.si`（上一轮） | 调用点在 `var x` **之前**、同一块 | **编不过**：`Variable x not in scope. (1516) ` | ⇒ 解析在**调用点**，且是**顺序敏感**的 |
| `F_nested_query.si`（本轮） | 调用点在顶层 def `mode_run` 里的嵌套 def 里，被查 def `probe_target` 在 root 体、引用 root 体的 `seen` | **编过**，且 `probe_target` 的两个标记都命中 | 调用点**自己**没有 `seen` ⇒ 链条要**往外走到最外层调用点**（root 体）才命中 |
| `F_nested_query_rt.si`（本轮） | 同上，`var seen = load(<U64>, scratch)`（防常量折叠） | **编过**（8706 B） | 同上，不是折叠假象 |

⇒ 两条候选机制里只剩一条：

- ~~root 体的 `var` 是全局~~ —— 被 K 否掉（是全局的话调用点在哪都能命中，K 不该报错）；
- **调用点链**（动态作用域）—— 与本项目「参考实现按调用点展开/实例化嵌套 def」一致（§6 第 1 条）。

我们的缺口：这层链条断在**顶层 `def` 的边界**（`call_user`/`ensure_def_fn` 是纯函数，不带捕获），
而 110 的链路是 root 体 → `mode_run`（顶层 def）→ `handle_test_result` → `get_input_switched`。
要真做，得把顶层 `def` 也闭包化（在调用点算捕获、按调用上下文实例化，`fns_` 的键要带捕获签名）——
**是机制改动，影响全部 98 份 + rv32im，要主程定**。8 份名单：110 / 075 / 069 / 112 等（`coverage.py`
的分类表：`未定义的名字 seen/test/memory/over`）。

## 4. 复现

```bash
python simplexc/tools/build.py simplexc_cli
python simplexc/tools/m5/coverage.py
python simplexc/tools/check.py
python work/tmp/ul14/refcc.py work/tmp/closure/F_nested_query_rt.si \
    work/tmp/closure/F_nested_query_rt.ref.bin --markers 1111111111111111,2222222222222222,3333333333333333
SG=/c/Users/Furan/.pi/agent/npm/node_modules/@ast-grep/cli/ast-grep.exe
"$SG" scan --config work/tmp/in_scope/lint/sgconfig.yml simplexc/lower/lower.cc simplexc/lower/lower.hh
```

## 5. 撞到的**别的**缺口（只分类，没动）

| 条数 | 错误 | 例 | 属于 |
| --- | --- | --- | --- |
| 23 | `CompileFail/IR 校验失败：Both operands to ICmp …`（`%sc = icmp eq ptr %ld, i64 0`） | 001 | 新暴露：U-L4 之后的下一环（改前 001 停在 `halt`，同一份代码在 `git stash` 掉本次改动后也停在 `halt`，见 §4 复现） |
| 14 | `未定义的函数 random` | 034 | 内建 |
| 12 | ParseError `表达式里出现意外的 '{'` | 147 | 前端裸块 |
| 11 | `未定义的名字 #CORRECT_OUTPUT` | 008 | 其它 |
| 4 | `未定义的名字 test` | 075 | §3 的 root var 类 |
| 3 | `未定义的函数 array` | 084 | 内建 |
| 2 | `clz` / `ror` / `input` / `memory` | 131/178/104/069 | 内建 + §3 类 |
| 1 | 073 一行 `tick 1: extern 条数 1 vs 0` | 073 | 已知的 extern 条数类（不是回归：073 改前是 cli_fail） |

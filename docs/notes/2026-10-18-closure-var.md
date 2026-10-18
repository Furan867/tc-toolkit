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
每个片段一个进程（参考编译器遇错 abort 整个进程）。片段在 `work/tmp/closure/`（E/K 在 `work/tmp/in_scope/`）。

| 片段 | 形状 | 参考实现 | 说明了什么 |
| --- | --- | --- | --- |
| `rootvar_E_def_then_var.si`（上一轮） | root 体：def（引用 x）→ `var x` → 调用 | 编过 | 解析**不在** def 的文本位置（那里 x 还没声明） |
| `rootvar_K_guard_before.si`（上一轮） | 调用点在 `var x` **之前**、同一块 | **编不过**：`Variable x not in scope. (1516) ` | ⇒ 解析在**调用点**，且是**顺序敏感**的 |
| `F_nested_query.si`（本轮） | 调用点在顶层 def `mode_run` 里的嵌套 def 里，被查 def `probe_target` 在 root 体、引用 root 体的 `seen` | **编过**，且 `probe_target` 的两个标记都命中 | 调用点**自己**没有 `seen` ⇒ 链条要**往外走到最外层调用点**（root 体）才命中 |
| `F_nested_query_rt.si`（本轮） | 同上，`var seen = load(<U64>, scratch)`（防常量折叠） | **编过**（8706 B） | 同上，不是折叠假象 |

三条证据各自的复现命令（第二条按 aborts 的报错看 stderr）：

```bash
# E：编过
python work/tmp/ul14/refcc.py work/tmp/in_scope/rootvar_E_def_then_var.si /dev/null
# K：编不过，stderr 出 `Variable x not in scope. (1516)`
python work/tmp/ul14/refcc.py work/tmp/in_scope/rootvar_K_guard_before.si /dev/null
# F：编过 + 两个标记都命中（跑的是 _rt 那版，值来自运行期 load）
python work/tmp/ul14/refcc.py work/tmp/closure/F_nested_query_rt.si \
    work/tmp/closure/F_nested_query_rt.ref.bin \
    --markers 1111111111111111,2222222222222222,3333333333333333
```

三条合起来 ⇒ 两条候选机制里只剩一条：

- ~~root 体的 `var` 是全局~~ —— 被 K 否掉（是全局的话调用点在哪都能命中，K 不该报错）；
- **调用点链**（动态作用域）：自由变量在**调用点**解析、顺序敏感；调用点自己查不到就**往外走**
  （走到最外层调用点）。与本项目「参考实现按调用点展开/实例化嵌套 def」一致（`in-scope-fix.md` §6 第 1 条）。

**主程已定：不要「把 root 体 `var` 当全局」（它会把参考会拒绝的程序变成我们接受 —— K 的报错就是
反例，属语义分叉），按 (a) 顶层 def 闭包化的方向做；但 (a) 不由本轮继续做**（超预算，
且是「改所有函数签名」级别的机制改动）。

我们的缺口：这层链条断在**顶层 `def` 的边界**（`call_user`/`ensure_def_fn` 是纯函数，不带捕获），
而 110 的链路是 root 体 → `mode_run`（顶层 def）→ `handle_test_result` → `get_input_switched`。
8 份仍停在 `未定义的名字 seen/test/memory/over`（110/075/069/112 等，分类见 `coverage.py`）。

## 4. (a) 的机制设计要点（**为下一位写者准备，本轮未实现**）

1. **捕获在哪算**：在**调用点**。把本轮 `collect_captures` 泛化成一个吃「body + own 名字表 +
   调用点作用域」的走查：名字①在调用点是 `Local`/`Capture` ⇒ 收进捕获表；②绑到 `Def`（兄弟嵌套
   def）或能在 `root_defs_` 里查到 ⇒ 递归摊平（已有）；③绑到**顶层 `def`**（`defs_`）⇒ **新加**：
   递归进它的体，把它那一层的捕获也算上（这就是「链条往下传」）。`visiting` 集合防互调环。
   走查只需要**名字 + 类型**（不需要 LLVM 值）；指针到生成调用时才从调用点取。
2. **`fns_` 的键怎么带捕获签名**：现在顶层 def 的键是 `(1, name, lo, "")`（第 4 位空串）。
   把第 4 位用作**捕获签名**（形如 `seen:S64,x:U8`，按首次出现序，稳定；空捕获 = `""`）。
   空签名与今天的键**完全相同** ⇒ 同一份 `Fn` 复用（见第 3 点）。签名里带**类型**而不只是名字：
   同名在不同调用点绑不同宽度时，IR 函数类型不同，键必须能分开。
3. **能否做到「无捕获的顶层 def 签名/ABI 逐字节不变」？能** —— 捕获表是从被调体里**真的被引用、
   且在调用点绑到栈槽/捕获的名字**算出来的：一个 def 的自由名全部落在模块级（`globals_`/`consts_`/
   前缀/枚举/顶层 `def`）时捕获表为空 ⇒ 追加参数个数 0、`LLVMFunctionType` 不变、调用点的实参不变、
   函数体里没有额外的 `Binding::capture` ⇒ IR 与今天**逐字节相同**（同键同 `Fn`，连生成顺序都不变）。
   所以「统一改所有函数」并**不**必要；只有传递闭包里碰到 root 体本地的那些 def 会换签名。
   （生成顺序注意：算捕获要在 `ensure_def_fn` 里、`LLVMAddFunction` **之前**完成，现在的结构已经满足。）
4. **怎么摊平传递**：声明参数在前、捕获指针追加在后（与本轮的 `ensure_nested_fn` 同一形状）；
   函数体里把捕获绑成 `Binding::capture`（只 raw 指针，不复制内存），于是它再调别的 def/nested def 时
   `lookup` 就能找到，按同样规则继续往下传。
5. **不要碰的地方**：`decl_param_types`（重载解析/`pick_def` 只认声明参数）与 `static_type` 的调用分支；
   额外参数只由 `call_user`/`ensure_def_fn` 知道。
6. **已知风险（实现时先解决）**：互递归的顶层 def（A→B→A）在「按签名实例化」下可能来回震荡 ——
   要么对强连通分量取**捕获并集**再实例化，要么把实例键换成「最外层调用上下文」。

**影响面清单**：

| 项 | 影响 |
| --- | --- |
| 动的函数 | `call_user` · `ensure_def_fn` · `collect_captures`（泛化）· `Fn`/`fns_` 的键 |
| 产物会变的份数 | 只有「顶层 def 传递地引用 root 体本地」的那些 —— 就是那 8 份（110/075/069/112 等），其余 90 份与今天逐字节相同（第 3 点） |
| `rv32im` / 176/177/180 | 今天能编过 ⇒ 没有那种形状 ⇒ 预期产物不变（**未验**：实现时要拿「改前改后产物 md5 相同」当判据，不要靠这句话） |
| 验收闸门 | `check.py` 5 PASS（`lower_equiv` rv32im 10 tick 差异 0）· `coverage.py` 的 equal 份数不减 |

## 5. 复现

```bash
python simplexc/tools/build.py simplexc_cli
python simplexc/tools/m5/coverage.py
python simplexc/tools/check.py
python work/tmp/ul14/refcc.py work/tmp/closure/F_nested_query_rt.si \
    work/tmp/closure/F_nested_query_rt.ref.bin --markers 1111111111111111,2222222222222222,3333333333333333
SG=/c/Users/Furan/.pi/agent/npm/node_modules/@ast-grep/cli/ast-grep.exe
"$SG" scan --config work/tmp/in_scope/lint/sgconfig.yml simplexc/lower/lower.cc simplexc/lower/lower.hh
```

## 6. 撞到的**别的**缺口（只分类，没动）

| 条数 | 错误 | 例 | 属于 |
| --- | --- | --- | --- |
| 23 | `CompileFail/IR 校验失败：Both operands to ICmp …`（`%sc = icmp eq ptr %ld, i64 0`） | 001 | 新暴露：U-L4 之后的下一环（改前 001 停在 `halt`，同一份代码在 `git stash` 掉本次改动后也停在 `halt`，见 §5 复现） |
| 14 | `未定义的函数 random` | 034 | 内建 |
| 12 | ParseError `表达式里出现意外的 '{'` | 147 | 前端裸块 |
| 11 | `未定义的名字 #CORRECT_OUTPUT` | 008 | 其它 |
| 4 | `未定义的名字 test` | 075 | §3 的 root var 类 |
| 3 | `未定义的函数 array` | 084 | 内建 |
| 2 | `clz` / `ror` / `input` / `memory` | 131/178/104/069 | 内建 + §3 类 |
| 1 | 073 一行 `tick 1: extern 条数 1 vs 0` | 073 | 已知的 extern 条数类（不是回归：073 改前是 cli_fail） |

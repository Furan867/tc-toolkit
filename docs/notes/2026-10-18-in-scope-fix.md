# U-L14 落地：`in_scope` 判定 + 程序根嵌套 def 的可见性（两处改动，一处一层）

日期 2026-10-18 · 改 `simplexc/lower/{lower.hh,lower.cc}` · 规格 = `docs/notes/2026-10-18-ul14-in-scope.md` §0/§5
现场：`work/tmp/in_scope/`（含 `commit1.msg`/`commit2.msg`、改前后的产物、`lint/`）

**结论先行**：两处改动本身都按规格落地、各自的片段判据都过；但**把守卫块真正编进去之后，语料上暴露出一串
与 `in_scope` 无关的缺口**（复合字面量字段初始化 48 份、嵌套 def 互调 9 份、`random`/`array`/`ror` 16 份、
程序根本地变量被 hoist 出去的 def 引用 8 份 ……）。98 份口径因此从 `10 全等 + 60 TickDiff + 28 cli_fail`
变成 **`3 全等 + 95 cli_fail`**。这两处改动要不要留在树上、以及下面那串缺口谁先做，等主程定。

## 1. 改动一：`in_scope` 判定（`6e09a16`）

`in_scope_const()` 原来只查顶层 `def`（`defs_` 只装 `ast_.defs`）。改成规格 §0 的三档：
① `Program` 构造时整单元预扫的名字表（顶层 `def`/`type`（枚举类型名 + 成员名）/`const #`，
外加程序根体的整棵语句树 —— 块递归、**显式不钻 `NK::Def` 的体**）∪ ② `lookup()`
（参数 / 局部 / 同体嵌套 def）∪ ③ 前缀表（`is_prefix_func()` + `prefix_globals()`）。
查询串带不带 `#` 都算；`var` **不进**名字表（它的可见性跟声明顺序走 —— V1b/V1c 判假要保真）。

判据片段（报告 §5 预备的那份，两边都能编）：

```text
                控制标记 1111…   探测标记 2222…
我们的产物 改前        1 命中            0            ← 修的就是这个
我们的产物 改后        1 命中            1 命中
参考编译器            1 命中            1 命中
```

## 2. 改动二：程序根语句树里的嵌套 def 整单元（含前向）可见（`c15468c`）

`in_scope` 判真之后，`if in_scope("get_input") { … get_input(…) }` 第一次真的被 lowering，
而这些调用点排在声明**之前**、有的还在别的顶层 def 体里：

| 语料 | 调用点 | 声明点 |
| --- | --- | --- |
| 006 | `mode_run` 体里第 295 行 | `run_sim` 体里第 394 行 `def get_input(cycle: Int) Input { }` |
| 058 | 第 269 行 | 第 350 行 |

U-L14 §2 的 T8（引用点在另一个顶层 def 里）/U1（引用点在别的 def 体里）两条都判**真**
⇒ 程序根语句树里的嵌套 def 与顶层 def 同档：整单元可见、且前向可见。
实现复用改动一的那次预扫（`collect_root_decls()`）把名字收进 `root_defs_`，
`call_named()` 在「顶层 def / 词法作用域」都查不到时按名取它（`Binding::nested` + `call_nested`，按需生成）。

判据：改前 68 份报 `未定义的函数 get_input / get_input_switched`；改后**这一条错误在 98 份里消失**，
它们停到下一类根因上（见 §3）。177 从 cli_fail 回到 equal。

## 3. 暴露出来的缺口（**本任务只报告，没动**）

改后 95 份 cli_fail 的失败分类（`python simplexc/tools/m5/coverage.py`）：

| 条数 | 错误 | 例 | 属于 |
| --- | --- | --- | --- |
| 48 | 带字段初始化的复合字面量（U-L4 布局未定） | 001 | **新语义**（struct 布局），别的子系统 |
| 12 | ParseError「表达式里出现意外的 `{`」 | 147 | 前端裸块 |
| 11 | 未定义的函数 `random` | 034 | 内建（路线 A） |
| 9 | 嵌套 def 调用另一个嵌套 def（`halt`/`is_wall`） | 002 | 闭包捕获传递（见下） |
| 8 | 未定义的名字 `seen`/`test`/`memory`/`over` | 110/075/069/112 | 程序根本地变量 + hoist 的 def（见下） |
| 3 | 未定义的函数 `array` | 084 | 内建 |
| 2 | 未定义的函数 `ror` | 178 | 内建/另一个 worker 的 patch |
| 1 | `ui_set_text()` 的重载解析拿不到参数类型 | 090 | 重载 |
| 1 | 空数组字面量 | 157 | 字面量 |

两条**看起来在本次这条可见性链上、但不是 <20 行能收的**，需要主程定：

1. **「嵌套 def 调用另一个嵌套 def」（9 份）**：`captures_of()` 撞到「被调名字绑着一个同体的 `Def`」时
   直接抛（`lower.cc` 的 `Binding::Kind::Def` 分支）。要真做得把兄弟 def 的绑定带进按需生成的函数，
   而兄弟 def 自己还可能捕获**外层本地的 alloca**（006 的 `halt` 捕获 `burst_target_cycle`）——
   于是还得把外层本地一路传下去（传递捕获 / 闭包转换），是机制改动。
2. **「未定义的名字 `seen`」（8 份）**：110 的 `def get_input_switched() { … seen … }` 是 `run_sim` 体里的
   嵌套 def，`var seen = 0` 也声明在 `run_sim` 体里**但在调用点之后**（调用 3222 行、声明 3676 行）。
   `captures_of()` 在调用点按作用域找 alloca → 找不到 → 生成出来的函数体里 `seen` 无解。
   ⇒ 参考实现既然编得过，它多半**不把程序根的 `var` 当本地 alloca**（当全局 / 或整体 hoist）。
   这条是**架构口径**问题，不是补几行：需要一次片段实验（`run_sim` 体里 `var x` + 引用它的嵌套 def
   + 在声明之前调用它 → 看参考产物里 `x` 是数据段还是栈槽）。

## 4. 判据账（原始输出在 `work/tmp/in_scope/`）

| 判据 | 要求 | 实测 |
| --- | --- | --- |
| 1 片段 | 我们与参考同判为真 | **过**：控制 + 探测标记都命中（改前探测缺失） |
| 2 `check.py` | 5 PASS（`hook_offline` 8/8） | **不过**：4 PASS · 1 FAIL（`hook_offline` 4/8 —— 它的 ours 系 case 编的就是 058.si，停在 `halt`；`lower_equiv` PASS、rv32im 10 tick 差异 0） |
| 3 全量 98 份 | 40 份 TickDiff 归零 | **不过**：`3 全等 + 95 cli_fail`（改前 `10 + 60 + 28`） |
| 4 不减份（主程改后口径：≥10 且含 002/004/006/020/026/038/058/177） | 8 份回来 | **不过**：只有 177 回来；其余 7 份停在 `halt`；另 176/180 仍 equal |
| 5 pi-lens | 新增代码命中 0 | **过**：C++ 规则（`cpp-no-printf` / `cpp-no-malloc-free`）改前改后都 0 条；`lint/probe.cc` 证明这个入口真的会报（同一套命令报了 2 条） |

`python simplexc/tools/check.py` 全量：`corpus` / `host_selftest` / `lower_equiv` / `probe_006` PASS，
`hook_offline` FAIL（JSON 见 `work/tmp/in_scope/check2.json`）。

## 5. 复现

```bash
python simplexc/tools/build.py simplexc_cli
./build/simplexc_cli.exe --src work/tmp/ul14/O1_two_sided_repro.si --out work/tmp/in_scope/O1.after.bin
python -c "import pathlib,struct;b=pathlib.Path('work/tmp/in_scope/O1.after.bin').read_bytes();print(b.count(struct.pack('<Q',1111111111111111)), b.count(struct.pack('<Q',2222222222222222)))"
python simplexc/tools/check.py
python simplexc/tools/m5/coverage.py
/c/Users/Furan/.pi/agent/npm/node_modules/@ast-grep/cli/ast-grep.exe scan --config work/tmp/in_scope/lint/sgconfig.yml simplexc/lower/lower.cc simplexc/lower/lower.hh
```

# 裸块 `{ … }`：参考行为实测 + 前端实现（lowering 未动，属另一写者）

诊断在 `docs/notes/2026-10-18-clifail-diagnosis.md` §1（12 份：147/149/159/162/163/164/165/167/168/171/172/174）。
本次只做**前端**（`simplexc/frontend/**`）；`simplexc/lower/**` 是另一个 worker 的唯一写区，未碰。
片段与产物全部在 `work/tmp/bareblock/`；参考侧器材 = `work/tmp/m5interp/referr.py`（内层 `work/tmp/m5gap/tcdll.py`，只读）。

## 参考行为实测（`work/tmp/bareblock/b*.si`）

判据量程的**硬限制**：参考对带 `run_sim` 根的小片段仍返回固定 8581 B 桩（`entry=0x212f`）——
`probe()` 是常量、整程序被折叠成空程序。所以这一层只有**接受 / 拒绝 + 报错文本**，拿不到运行期值。
下面每条都有对照片段，把「接受」与「浮于接受」的情况分开。

| 片段 | 构造 | 参考 |
| --- | --- | --- |
| `b01_scope.si` | 块内 `var z`，块外 `return y + z` | **失败** `Variable z not in scope.` |
| `b01_ctrl.si` | 同样的事，但块是 `if true { … }` | **失败**，同一句 `Variable z not in scope.` |
| `b02_shadow.si` / `b13_shadow_leak.si` | 块内 `var y = 100` 遮蔽外层 `y` | 成功 |
| `b09_redecl.si` | 同一作用域里 `var y` 两次 | 失败 `y is already declared in this block.` |
| `b03_break.si` | `while { { i=i+1; if i==3 { break } } }` | 成功 |
| `b12_label.si` | 块内 `break outer`（带标签） | 成功 |
| `b04_return.si` | 块内 `return n + 1` | 成功 |
| `b05_empty.si` | 空块 `{}` | 成功 |
| `b06_nested.si` | 裸块套裸块 | 成功 |
| `b10_root.si` | 程序根体里的 `{ }` | 成功 |
| `b11_expr.si` | `var q = { var z = 1 }` | **崩**：`front_end.nim(2728,13) 'false' var/tok_var_dec [AssertionDefect]` |

结论（四条，都是「怎么验的」写在一起）：

1. **裸块 = 一层作用域**：块内声明出块即不可见（`b01_scope`），且与 `if` 块的报错**逐字相同**（`b01_ctrl`）
   → 参考把裸块和 if/while 体当同一类块，不需要新语义。
2. **允许遮蔽**：`b02/b13` 成功，而**同作用域**重复声明是硬错（`b09`）→ 是「子作用域遮蔽」，不是「重复声明被静默吃掉」。
3. **`break` / `return` 穿透**：它们是语句位置的关键字，块不构成循环/函数边界（`b03/b04/b12` 成功）。
4. **`{` 不是表达式**：`b11` 参考自己断言崩溃；我们的报错是友好的 `表达式里出现意外的 '{'`（`parser.cc:723` 那条兜底）
   —— 保持现状，不比照崩溃。

## 实现（前端，33 行）

| 文件 | 改动 |
| --- | --- |
| `frontend/ast.hh` | 新 `NK::BlockStmt` + `struct BlockStmt { std::unique_ptr<Block> block; }` |
| `frontend/parser.cc` | `parse_statement` 开头加 `{` 分支（复用 `parse_block()`）；`node_py_name`、`is_stmt_kind` 各补一条 |
| `frontend/frontend.cc` | `walk_nodes` 补 `BlockStmt`（**这个 switch 的其它分支没有 default，漏了会让可达闭包/覆盖统计漏掉块内一切**）；`close_line_of` 返回内层 `Block::close_line` |
| `frontend/ast_dump.cc` | `node_to_json` / `render_tree` 两个 switch 各补一条（都是穷尽式、无 default） |

`is_stmt_kind` 那条不是可有可无：`ast_dump --all` 的 `stmts` 列靠它计数（`frontend.cc:448`）。

## 判据

`python simplexc/tools/check.py` → **5 PASS / 0 FAIL / 0 SKIP**（34.2 s，`work/tmp/bareblock/…` 之外的日志见
`work/tmp/check/`）。仓库里那份 98 份报告未被覆盖（本次产物全在 `work/tmp/bareblock/`）。

12 份的 `ParseError` 全消失（`work/tmp/bareblock/coverage_12.tsv`，`--ids 147,149,159,162,163,164,165,167,168,171,172,174`）：
12/12 由 `ParseError 表达式里出现意外的 '{'`（147 报在第 1064 行）变成
`Unsupported：未定义的函数 array @ 第 {806,1847,1951,1859} 行` —— 即链条推进到 `array`（诊断 §1 预测的下一环）。
12 份里语句位置的 `{` 共 **37 处**（147 有 4 处，其余各 3 处，判据 `grep -cE '^\s*\{\s*$'`）。

全量 98 份（`work/tmp/bareblock/coverage_98.tsv`，同一棵树的混合状态，见下）：**19 全等 / 9 diff / 71 cli_fail**，
基线报告里那 15 份全等**一份没掉**（差集为空）；分类里已无 `ParseError`。
⚠ 这一跑的绝对数不是「本次改动」的效果：工作区里 `simplexc/lower/lower.cc` 有另一个 worker 的**未提交**改动
（`git status` 显示 `M`），早先提交的嵌套 def 捕获（`ebc97c2`）等也在起作用 —— 那份报告是 `adb2e25` 前的口径。
本次能确定性归因的只有：15 份全等不减、12 份 ParseError 消失。

pi-lens 命中：入口照 `docs/notes/2026-10-18-ir-build-lint.md`（ast-grep 0.45.3 + pi-lens 的 298 条规则，
`work/tmp/bareblock/lint/`）。4 个改动文件**改前 0 / 改后 0**（其中 C/C++ 档只有 `cpp-no-printf`、
`cpp-no-malloc-free`、`c-no-malloc-free` 三条）。入口忠实性用探针验：临时 `ptest.cc`（含 `printf`）命中 1 条，
且 `ast-grep run -p 'std::make_unique<BlockStmt>($$$)'` 能看见新代码（`parser.cc:311`）。

## 遗留：lowering 必须补什么（另一个 worker）

前端现在产出 `NK::BlockStmt`，`lower_stmt` 的 default 是 `throw`，所以**裸块一进 lowering 就报
`UNSUPPORTED: 语句形态 ? @ 第 N 行`**（最小复现 `work/tmp/bareblock/b01_scope.si`，报第 3 行；
`?` 说明 `kind_name` 也不认识它）。要补四处，语义按上面的实测：

| 位置（`simplexc/lower/lower.cc`） | 补什么 |
| --- | --- |
| `kind_name`（`:421` 起，末尾 `return "?"`） | `case NK::BlockStmt: return "BlockStmt";` |
| `lower_stmt`（`:881` 那组 case） | `case NK::BlockStmt: lower_block(*static_cast<const BlockStmt&>(st).block); break;` —— `lower_block`（`:872`）自带 scope push/pop，正是实测语义，**不需要额外处理 break/return**（它们本来就是语句） |
| `for_each_child`（`:65` 起，`default: break` **静默**） | 下钻 `block` —— 不补则 `captures_of`（闭包捕获表）漏掉裸块里的捕获 |
| `lower.cc` 的 `kind_name` / `for_each_child` 都不是 `lower_stmt` 那种安全 default | 上面两条各自独立，别只补一条 |

跨文件提醒：`frontend.cc` 的 `walk_nodes` 那份静默 default 本次已补；`ast_dump.cc` 两份是穷尽式，也补了。

## 未覆盖 / 风险

- 试出来的作用域与遮蔽结论只有「接受/拒绝 + 报错文本」两层，**没有运行期证据**（8581 B 桩，见上）。
  若要运行期结论，得让片段产生不可折叠的副作用（写 MMIO），本次没做。
- 12 份里的裸块**是否都被 lower 到**没验：147 的裸块在第 1063 行之后，而 CLI 只报第一个 `Unsupported`
  （`array @ 806`），所以「补完 lowering 这 12 份就能过」这条**没验**——补完还要看块内是否有别的缺口
  （如嵌套 def / `array`）。
- `parse_statement` 的 `{` 分支只认**语句位置**的 `{`：`label: {` 之外的一切表达式位置的 `{` 仍走
  `primary` 的兜底报错（与参考的断言崩溃不同，见 `b11`）。
- 与 Python 原型 `frontend/expr.py:588` 出现**有意**的前后端差异：原型没有裸块（原型只读，未动）。
  `ast_dump` 的 `--stats` 会把 `BlockStmt` 计入 `stmts`（`b05_empty.si` 的 `f` = 3 条），原型没有这一类比对的输入。

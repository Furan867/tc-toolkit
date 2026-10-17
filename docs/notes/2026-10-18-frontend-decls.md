# 前端第一步：声明表 + 花括号配平（M3.0，2026-10-18）

工具：`simplexc/frontend/decls.py`（单文件，只用标准库；不编译、不加载 DLL、不跑游戏）。
范围就是 `docs/status.md`「下一步」第 1、2 条的交集：**只到「声明 + 花括号配平 + 名字级可达性」**，
不解析表达式、不做类型、不生成 IR。

```
python simplexc/frontend/decls.py --src simplexc/corpus/rv32im/rv32im.si            # TSV 到 stdout
python simplexc/frontend/decls.py --src <file> --json out.json                       # 完整结果
python simplexc/frontend/decls.py --src <file> --roots lenient                       # 复现 dossier 口径
```

## 0. 结论（先看这几条）

1. **顶层构造只有五种**：`type`（Enum/Struct）、`const`、`var`、`def`（含嵌套）、带标签的块
   （`run_sim: while true`）。98 份 `corpus/src/*.si` + `rv32im.si` 全部跑通，**未支持项 0 条**、
   代码行覆盖 **100%**（每份文件都是 100%）。
2. **花括号配平必须做、但它自己够用**：掩码后配平，`rv32im.si:154` 那个多余的 `)` 只产生
   一条警告（`get_ram_value` 圆括号 −1），不报错、不崩 —— 与 §7.1 的实测一致
   （参考实现对不可达 def 体只看结构）。
3. **dossier 的「19 个可达」不是调用图闭包，是 10 个**。差 9 个名字，全部来自
   `work/tmp/dossier/reachability.py` 的一个口径：它把 `def name(` 的**声明头当成了调用**
   （单行 def 8 处 + 嵌套 def 头 1 处）。用 `--roots lenient` 能一字不差复现 19；按真入口
   算（`run_sim` 块 + `tc_main`）是 10。**M3.0 的规模只会更小，不会更大**，但 `docs/status.md`
   里的「19 / 5200 行」需要按这个口径重述（见 §5 对账）。
4. 被 dossier 算进「可达」的 9 个名字里，只有 `handle_test_result`（13 行）与
   `get_component_count`（两处声明共 5 行）有实质的体，其余 7 个是空体或单行 getter。
   口径差异对 lowering 工作量的影响可以忽略，但对「我们到底该编译哪些函数」这句话必须说准。

## 1. 输出长什么样

TSV，一行一个声明，首行表头：

`kind | name | lo-hi | params | ret | body_parsed | reachable | depth | enclosing | note`

| 列 | 含义 |
| --- | --- |
| `kind` | `type` / `const` / `var` / `def` / `label`（带标签的块）/ `pp_if`（顶层 `#if`） |
| `lo-hi` | 行范围（含头行与收尾 `}` 行；单行声明只写一个数） |
| `params` / `ret` | `def` 的参数表与返回类型，**原文片段**（不做类型解析）；其它 kind 为 `-` |
| `body_parsed` | 本阶段体**只配平不解析**：非空体一律 `否`，只有空体（`{ }`）是 `是`。它**不代表**体是否闭合 |
| `reachable` | 仅 `def` 有值；默认 strict 口径（见 §5） |
| `depth` / `enclosing` | 花括号深度与最近外层声明名 —— 嵌套 def（闭包）与顶层 var 靠这两列区分 |
| `note` | `Enum`/`Struct`、`nested`、`paren±N` |

stderr 是统计（行数、覆盖、可达、警告、未支持），`--json` 里是同一份数据的结构化版本
（含未覆盖行号清单）。

## 2. 语法摸底：顶层构造清单（rv32im.si，10369 行）

| kind | 形态 | 条数 | 例子 |
| --- | --- | --- | --- |
| `type … Enum[…]` | `type SimCommand Enum[run, refresh, …]`（单行，成员可上百个） | 5 | L16–18, L44, L57 |
| `type … Struct {` … `}` | 多行块 | 2 | L220–221, L223–224（**体是空的**） |
| `const #NAME = …` | `= Ptr <addr>` 16 条 / `= 0` 1 条 | 17 | L11–14, L32–43, L165 |
| `var name = …` | `Ptr <addr>` 5 / `raw_cast(<Arr[U64]>, …)` 2 / 空复合字面量 2 / 带类型前缀（`U64 0`、`Time 0`）3 / 纯标量（`-1`、`false`）8 | 20 | L3–9, L140–145, L184–187, L226–227, L10285 |
| `def` | 多行体 36（其中 5 个是 `{` `\n}` 空体）/ 单行体 8（6 个有代码 + 2 个 `{ }` 空体） | 44 处 / 43 个名字 | L20, L59, L229, L6482 |
| 带标签的块 | `run_sim: while true {` … `}` | 1 | L10287–10368 |
| 顶层 `#if` | `#if in_scope("name"){` … `}` | **0** | 语料里 3 处 `#if` 全在 def 体内 |

- 44 处 def 里有 5 处是**嵌套 def**（闭包）：`halt`(L6523)、`handle_test_result`(L6530)、
  `get_target_cycle`(L10262) 在 `mode_run` 里；`get_input_switched`(L10291)、`check_output`(L10293)
  在 `run_sim` 里。它们只有在嵌套 def 里才出现的名字（`halt`/`handle_test_result`）不能当全局符号处理。
- `get_component_count` 有**两个声明**（L63 单行、L64–67 带参）—— 重载，按名字算 1 个节点。
  这是「44 处声明 / 43 个名字」的唯一来源。
- **摸底修正**：`m3_0_constructs.tsv` 里「`const #COUNTS = [ … ]`（大数组常量）」标的是 `preamble:1`，
  实际在 L65 —— 它是 `get_component_count(component_type)` **函数体内**的 const，不是顶层声明。
  （那条的正则 `^\s*const #\w+\s*=\s*\[` 带前导空白，拦到的是局部声明。）
- 顶层再没有别的东西：`}` 只作为上一个声明的收尾行出现，没有裸语句、没有 `import`。

## 3. `#if` 与标签循环怎么处理

**`#if in_scope(...){ … }` 什么都不用做**：它长得像 `if`，行内带 `{`、块尾有 `}`，花括号配平天然
收口，所以它对手头的三个问题（行范围、配平、调用边）都透明。本文件 3 处 `#if` 查的名字是
`on_ui_update` / `on_manual_input`，本文件里**没有这两个 def**（是否生成代码要看 `in_scope` 的判定，
依据 `facts.md:556` 的实测，本任务没重验），这条语义属于后面的语句层。

唯一的例外是**顶层** `#if`：它里面的 `var`/`def` 仍是声明，所以记成 `kind=pp_if` 的一块，
块内声明 `depth=1`、`enclosing=#if in_scope("…")`。本语料**没有**顶层 `#if`
（99 份文件全是 0 条），这条路径用 `work/tmp/decls/frag/top_pp_if.si` 造片段验证过。

**标签块**：`run_sim: while true {` 记成 `kind=label`，span 10287–10368。`break run_sim`
是语句层语义，本阶段不碰。反过来，标签块的存在决定了一条规则：**声明层 = 所有未闭合声明都是
`pp_if`**。所以 `run_sim` 体里和 def 体里的 `var` 不会被误记成顶层声明（`var command`、
`var run_target_cycle`、`var i` 这些都在里面）。

## 4. 词法掩码：为什么必须先掩码再数括号

`mask_source()` 一次左到右扫描，产出两份等长文本：`raw`（只抹注释）与 `code`（注释 + 字面量文字
都抹成空格，反引号模板保留 `{expr}`）。行号一直对得上原文。语料实测：

- 花括号：掩码前后都是 402/402 —— **99 份文件里 0 份**有花括号藏在注释/字面量里。
  所以单看配平，裸扫也能过；掩码不是为了花括号。
- 圆括号：7387（原文）→ 7366（掩码后），**21 个括号藏在注释/字面量里**。不掩码的话
  「圆括号不配平」这条体检会全是假阳性（`rv32im.si:154` 那个真错会被淹掉）。
- 三种字面量都必须能吃：`"…"`、`'…'`（`label == '内存'`，只在死代码里）、
  `` `…` ``（4 处 `set_error(`halt`)`，无插值）。
- 陷阱：L32–43 的行尾注释里有 `"""`（`// """ & $c_alloc(4096) & "\n" & """`）。**先**识别
  `//` 再识别字面量，否则字符串扫描会跨行吃掉后面几十行。

## 5. 可达性算法与 dossier 的 19 对账

算法（`closure()`，纯名字级，不做重载/不做类型）：

1. **边**：一个 def 的「自己的行」= span 去掉嵌套声明的 span（嵌套 def 体不算外层的调用），
   在这些行上匹配 `(?<![\w.])(\w+)\s*\(`，排除关键字与 `def name(` 声明头。
2. **根**（strict）：顶层语句 —— `label`/`var`/`const`/顶层 `pp_if` 的自己的行 + 名字叫
   `tc_main` 的 def（本文件没有），再减去 def 的 span。本文件根 = `run_sim` 块。
3. BFS 求闭包。名字级匹配的误差方向可控：漏判调用 = 把活的当死的（危险），多判 = 把死的当活的（安全）。

结果（`rv32im.si`）：

| 口径 | 可达名字 | 可达 def 代码行 | 不可达名字 | 不可达代码行 |
| --- | --- | --- | --- | --- |
| **strict**（真入口） | **10** | 5090 | 33 | 101 |
| **lenient**（dossier `reachability.py`） | **19** | 5109 | 24 | 82 |
| dossier 记载 | 19 | 5109 + 顶层骨架 91 = 5200 | 24 | 82 |

**差异的 9 个名字**：`check_output`、`get_component_count`、`get_cycle`、`get_delay_score`、
`get_gate_score`、`get_input_switched`、`get_last_time`、`get_test`、`handle_test_result`。

原因逐条：

- 前 8 个的声明里都有一处**单行或者空体**（`get_component_count` 是重载里 L63 那个单行声明；
  `check_output`/`get_input_switched` 嵌在 `run_sim` 里也是单行）：`reachability.py` 判「函数体」
  用 def 栈，单行 def 在同一行就 pop 掉了，于是 `def get_cycle()` 这一行被当成顶层行收集、
  `get_cycle(` 被记成一次调用 → 自己成了根。
- `handle_test_result` 是**嵌套 def 头被算成外层调用的效果**：`reachability.py` 扫 def 体时
  用的是整个 span（含嵌套 def），而嵌套 def 的声明头 `def handle_test_result(` 就在 mode_run 的
  span 里，于是 mode_run（活）→ handle_test_result（活）。它在本文件里**从来没被调用过**。
- 两个口径下 `check_output` / `get_input_switched` 的差别还有一个现实含义：它们是
  `#if in_scope("check_output_switched")` 的被查名字，**是否生成代码取决于 in_scope 判定**，
  名字级调用图看不见这条边。所以 strict 的真实风险只有这一处，而它已经被 `#if` 块内的调用
  （块内若有调用，调用边是可见的）覆盖 —— 本文件的这两个名字确实没被调用，且体是空的。

对 M3.0 范围的影响：可达 def 代码行 5109 → 5090（−19 行），死代码 82 → 101 行（+19）。
**结论：范围只小不大**，`docs/status.md` 第 1 条的「5200 行」应改述为
「可达 def 5090 行 + 顶层骨架 91 行 = 5181 行（strict 口径）」。这条要主程折叠进账本。

## 6. 未支持清单

**空**。98 份 `corpus/src/*.si` + `rv32im.si`：未支持顶层构造 0 条、代码行覆盖 100%、退出码全 0。

明确**故意不做**的（不是漏了）：

| 不做 | 依据 / 影响 |
| --- | --- |
| 表达式、类型、作用域、常量求值 | 本任务的一刀切在这里（M3.0 第二步） |
| 不可达 def 的体（strict：33 个名字 / 34 处 / 101 行） | `docs/status.md` 第 2 条：只配平跳过并记账 |
| `Struct` 体的字段 | rv32im 里两个 struct 都是空体；非空时本工具只记 span，不解析字段 |
| `Enum` 的成员表 | 只记 span；`com_switch_bit`(42)、`com_switch_word`(40) 这些名字后面 lowering 要 |
| `const` / `var` 的初始化表达式 | 只记 span（16 条 `Ptr <addr>`、1 条标量、`raw_cast(<Arr[U64]>, …)`） |
| `#if in_scope` 的真假 | 只按块配平；3 处求值语义是语句层的事（U-L14） |

已知边界（会静默接受、后面必须补）：

- 花括号不配平 → 报错退出 1（见 §7）；但**圆括号不配平只警告**（这是设计，不是漏）。
- 名字级可达性不认 `in_scope` 的字符串参数（`#if in_scope("x")` 里的 `x` 被掩码抹掉）。

## 7. 破坏性测试（三个片段，`work/tmp/decls/frag/`）

| 片段 | 内容 | 结果 |
| --- | --- | --- |
| `missing_close.si` | `def f() Int {` 后没有 `}` | 退出 1：`花括号未配平：文件结束时还有 1 个 '{' 未闭合，第 3 行` |
| `extra_close.si` | 多一个 `}` | 退出 1：`第 4 行：多余的 '}'（此处没有未闭合的 '{'）` |
| `swallow.si` | 缺 `}` 导致后面的 `run_sim:` 被吞进 `def g` | 退出 1：未配平 + `其它未覆盖行：4, 6, 7, 8`（被吞的行不再被当成顶层声明，覆盖率掉到 25%） |

第三个片段同时验证了「被吞的语句不会假装成功」：它不会静默把 `run_sim` 记成 `g` 的嵌套声明。

## 8. 下一步：表达式解析从哪下手

体切片入口现成：`decls.py --json` 已给每个 def 的 `lo`/`hi`、参数、返回类型、是否可达。
建议顺序（按「谁的体最像普通过程代码」排）：

1. **语句层先在 `mode_run`（L6482–10283，1272 代码行）上做**：它只有 `if` / `while` /
   `var`/`let` / 赋值 / `store`/`load` / 嵌套 def，没有 `switch`，是最好调的骨架。
2. 再做 `switch` + `case … {}`（16/4 处，全在 `run_sim` 与 `mode_refresh` 里），
   因为它是 `mode_refresh`（L229–6480，3782 代码行）的结构主干。
3. `#if in_scope("…"){ … }` 在语句层当 `if` 先吃掉（结构上同形），真假判定等 U-L14。
4. 表达式优先级爬升最后做，卡点不在语法：U-L7（提升/截断，决定 5053 行模板代码怎么翻译）、
   U-L1（`load`/`store` 的 `p` 是字节地址、`align` 只能是 1）、`type_of(...)`/`@Size`/`@Type`
   （只出现在死代码里，M3.0 可以不实现但要能跳过）。
5. 词法上还欠的：`'…'` 单引号字面量的语义（U-L3）与 `<…>` 类型字面量的解析 ——
   rv32im 里出现的是 `<U1>`…`<U64>`（非 8 的倍数：U1/U3/U4/U5/U6/U7/U12/U20/U24…）与 `raw_cast`
   里的 `<Arr[U64]>`；掩码层已经能吃掉它们，AST 层要给出表示。

# 前端第二步：语句 + 表达式 → AST（M3.0，2026-10-18）

工具：`simplexc/frontend/expr.py`（单文件，只用标准库；不编译、不加载 DLL、不跑游戏）。
范围就是 `docs/status.md`「下一步」第 5 条的**离线表达式树通道**：把 `decls.py` 给的 `lo`/`hi`
切成 token 流，解析成**语法层** AST。**不生成 IR、不做类型检查、不做常量求值**。

```
python simplexc/frontend/expr.py --src <file.si> --def mode_run            # AST dump → stdout
python simplexc/frontend/expr.py --src <file.si> --def mode_run --stats    # + stderr 详细统计
python simplexc/frontend/expr.py --src <file.si> --def mode_run --json out.json
python simplexc/frontend/expr.py --src <file.si> --all                     # 全文件体检表（TSV）
```

`--def` 也接受**带标签的顶层块**（`run_sim`）——它是冻结接口 §1 里的程序根。
`--all` 的口径：可达 def 解析失败 → 退出 1；不可达 def 失败只记账（与 `decls.py` 的纪律一致）。

## 0. 结论（先看这几条）

1. **M3.0 的主体已经能解析**：`rv32im.si` 里 12 个有体的声明（`mode_refresh` 3782 代码行 +
   `mode_run` 1272 + 顶层 `run_sim` 块 45 + 小函数）**全部 100% 覆盖**，未支持构造 0 条。
   全文件 44 处 `def` 里只有 1 处失败：`get_ram_value`（不可达死代码，源里 L154 多一个 `)`——
   `decls.py` 对同一个 def 报的也是 `paren-1`）。
2. **语法层的信息一条没丢**：`CastExpr{target}` 把 `U32 (…)` 与 `S32 0` 这种字面量类型标注都保住了；
   二元/一元/三目一律不折叠。这是给 `dossier/div_sign.md` 指示 3 留的形状（下面 §5 有片段）。
3. **`in_scope("name")` 的字符串现在看得见了**：`decls.py` 只配平（字符串被掩码抹掉），
   `expr.py` 把它解析成 `PpIfStmt` 的普通调用 `in_scope(<str 字面量>)`，U-L14 落地时不用再回去扫源码。
4. **消歧规则是本次唯一的"发明"**，两条，都写成代码注释 + §2 的表格：并置（`T x`）只能是转换；
   `name (` 靠**有没有空白**分调用/转换（Nim 语法 `f (x)` 不是调用，实测语料 100% 符合）。
5. **边界（不是漏）**：模板字面量的 `{…}` 插值**报错退出**（U-L3 语义未定，不能猜）。
   99 份语料里 168 处 def 因此解析失败 —— 这是 M3.0 之外的东西，不要为了刷绿把它吃掉。

## 1. 输出长什么样

stdout = AST dump（缩进树，每行 `Kind @行号 字段=值`，子节点下钻），stderr = 统计：

```
simplexc/corpus/rv32im/rv32im.si mode_run  L6482-10283
  语句 1117 条（含嵌套 def 3 个）；表达式节点 5800 个
  代码行 1257（嵌套 def 另 15，span 合计 1272）；覆盖 1257/1257 = 100.0%
  未支持构造：无（不认识的构造一律报错退出，不静默跳过；整文件的清单看 --all）
  节点：TypeRef×2243 CastExpr×2220 IdentExpr×1657 BinaryExpr×949 …
```

- **覆盖率的定义**：分母 = 该 def span 内的非空代码行（去掉嵌套 def 的 span，单列）；
  分子 = 「产生过 AST 节点的行」∪「块的收尾 `}` 行」∪「复合字面量的收尾 `}` 行」。
  所以 `}` 单独占一行不会算成未覆盖，而**任何没被解析的行都会掉覆盖率**（`--stats` 会把它们连原文打出来）。
- `--json` 里是同一份数据的结构化版本（`stats` + `ast`，每个节点都有 `kind` 与 `src_line`）。
- 退出码：0 成功；1 解析失败 / 找不到 def；2 读不了文件。
- 错误一律报**行号 + 列 + 原文**，缺定界符时会指出**配对的那一行**：
  ```
  错误：第 4 行 第 1 列：这里要 ')'（配对第 3 行的 '('），看到 '}'
        4 | }
  ```

## 2. 语法要点（这一节是给下一位写者看的）

### 2.1 运算符优先级（低 → 高）

| 级 | 运算符 | 结合 | 依据 |
| --- | --- | --- | --- |
| 1 | `\|\|` | 左 | 语料里只有 `082.si:1754` 一类条件 |
| 2 | `&&` | 左 | 93 处 |
| 3 | `==` `!=` | 左 | Nim 里比较同级 |
| 4 | `<` `<=` `>` `>=` | 左 | |
| 5 | `\|` | 左 | |
| 6 | `^` | 左 | |
| 7 | `&` | 左 | **比 `==` 紧**（Nim 的做法）；`U32 x & 1 == 0 ? a : b` 靠它读对 |
| 8 | `<<` `>>` | 左 | |
| 9 | `+` `-` | 左 | |
| 10 | `*` `/` `%` | 左 | `/` 与 `%` 同级左结合（`a / b % c` = `(a/b)%c`） |
| — | `? :` | 右 | 比二元都低，比赋值高（`x & 1 == 0 ? a : b` = `((x&1)==0) ? a : b`） |
| — | `= += -= *= /= %= &= \|= ^= <<= >>=` | 右 | **只在语句层**，不是表达式 |

前缀一元：`- + ~ !`（`!` 是逻辑非，语料 168 处 `!halt_run`）；后缀：`(args)` `[index]` `.member`。
**转换（cast）在前缀层**，操作数只到 unary 级 —— 所以 `U8 (U1 x) | y` = `(U8 (U1 x)) | y`，
`S32 0 - S32 7` = `(S32 0) - (S32 7)`（div_sign 的形状靠这条）。
⚠ 未决：`cast` 与位运算符的紧密度只用上位规则，语料里没出现过能区分的写法（见 §7）。

### 2.2 `T (…)` 是转换还是调用 —— 两条判据

| 源里的形状 | 读法 | 规则 |
| --- | --- | --- |
| `TestResult max(U64 a, U64 b)` | 转换（操作数是调用） | **并置**只有一种读法 → 转换 |
| `Arr[U8] []` | 转换（操作数是数组字面量） | 同上（`[` 需要名字是类型名） |
| `S8 -8` / `U1 ~(U1 x)` / `U1 !y` | 转换 | `-`/`+` 要名字是类型名；`~`/`!` 只有转换一种读法 |
| `U8 (x)`（**有空隙**） | 转换 | Nim 语法：`f (x)` 不是调用 |
| `min(x, y)`（**紧邻**） | 调用 | 同上 |
| `Input {` …（复合字面量） | 构造 | 名字是类型名 + 同行 `{` |

类型名表由三样拼成（`declared_type_names()`）：`type` 声明、每个 def 头的参数/返回类型字面、
内建 `Int/Bool/Str/None/Time/Ptr/Seed/Void/Uint/Sint/Arr` 与 `[US]\d+`（`U33`/`S8`/`U1`…）。
**跨行的并置不算转换**（`switch command` 后面一行 `case` 不会被吃掉）；
`T(x)` 紧邻而 T 是类型名时会照样当转换解析，同时记一条警告（实测 0 条）。

### 2.3 `#if in_scope("…")`、嵌套 def、标签循环

- `#if <expr> {…}` → `PpIfStmt`（跟 `if` 同形，但节点类型分开：真假判定是 U-L14，不在这里做）。
  `else`/`elif` 裸字与带 `#` 两种语法都吃；**绑定规则**：`else` 必须紧跟在 `#if` 那块的 `}` 之后
  （下一个 token）—— 所以 `if a { #if in_scope("b") {…} } else {…}` 里的 `else` 归外层 `if`，
  不会被抢走（语料里两种都出现过）。
- **嵌套 def 就是语句**：`mode_run` 里的 `halt` / `handle_test_result` / `get_target_cycle`
  解析成 `DefStmt`（带 `params`/`ret`/`body`），闭包捕获是 lowering 的事（那里要靠作用域链）。
  `--def halt` 也能单独 dump 一个嵌套 def。
- **标签循环**：`run_sim: while true {…}` → `WhileStmt(label='run_sim')`；
  `break run_sim` → `BreakStmt(label='run_sim')`。标签块本身用 `--def run_sim`（`form='label'`）。
- **`switch` 不配对花括号**：`switch expr` 后面跟 0 个或多个 `case X {…}`/`default {…}`，
  遇到不是 `case`/`default` 的 token 就结束（`run_sim` 里那处 4 个 case 就是这么收的）。

### 2.4 词法

`#NAME`（编译期常量）与 `@NAME`（`@Size`/`@Type`）是**独立 token 种类**，不是 `#` + 标识符。
数字：十进制 / `0x` / `0b`，允许 `_`（浮点是显式报错）。字面量三种：`"…"`、`'…'`、`` `…` `` —
**定界符保留在 `LiteralExpr.quote` 上**（`'…'` 的语义 U-L3 未定，`'内存'` 只在死代码里）。
`1.ms()` 是数字上的后缀成员调用；`input.len()` 是成员调用。模板里出现 `{…}` → 报错（U-L3）。

## 3. AST 节点表

位置字段：每个节点都有 `src_line`（1-based，取节点**第一个 token** 的行）；`Block` 另有
`close_line`，`ArrayLitExpr` / `CompositeLitExpr` 另有 `close_line`。`TypeRef` 是**源里的类型拼写**
（`name` + `at` + `elem`），**不是**冻结接口 §1 的 `Type`。

| 节点 | 字段 | `mode_run` 里的次数 |
| --- | --- | --- |
| `FunctionDef` | `name/form/params/ret/body/lo/hi` | 顶层 1 个 + 每个 `--def` 一次 |
| `Block` | `stmts/close_line` | 160 |
| `VarDeclStmt` | `kw(var/let/const)/name/init` | 667（`const #NAME = …` 同形，`name` 带 `#`） |
| `AssignStmt` | `target/op/value` | 204（`op` 是原文 `=`/`+=`/`&=`/`^=`…） |
| `ExprStmt` | `expr` | 84（只允许调用，见下） |
| `IfStmt` | `cases[IfCase]/else_block` | 149（`elif` 是同节点的多个 case） |
| `PpIfStmt` | `cases/else_block` | 1（`#if in_scope("on_ui_update")`） |
| `WhileStmt` | `cond/block/label` | 4 |
| `SwitchStmt` | `subject/cases[SwitchCase]` | 0（只在 `run_sim` 里有 1 个） |
| `ReturnStmt` | `value`（None = 裸 `return`） | 4 |
| `BreakStmt` | `label` | 1 |
| `DefStmt` | `name/params/ret/body` | 3（嵌套 def） |
| `LiteralExpr` | `kind(int/str/template)/text/quote/value` | 754 |
| `IdentExpr` | `name`（常量带 `#`） | 1657 |
| `CastExpr` | `target: TypeRef/expr` | 2220 ← `U32 (…)` 与 `S32 0` 都是它 |
| `UnaryExpr` | `op/expr` | 74 |
| `BinaryExpr` | `op/left/right` | 949 |
| `CondExpr` | `cond/then/else_` | 19 |
| `CallExpr` | `callee/args/type_args` | 125（`load(<U64>, p)` 的 `<U64>` 进 `type_args`） |
| `MemberExpr` | `obj/name` | 2（`1.ms()`）；`a.b()` = `CallExpr(callee=MemberExpr)` |
| `IndexExpr` | `obj/index` | 0；`run_sim` 里有 2（`input_replay[i] = …` 的左边） |
| `ArrayLitExpr` | `items/close_line` | 0；语料别处有 `[U8 195, …]`、`[]` |
| `CompositeLitExpr`/`FieldInit` | `typ/fields/name/value` | 0；`Input { a: …, }` 在多行上，尾随逗号也吃 |

**语句层的一条硬规则**：语句位置的表达式**必须是调用**（`ExprStmt` 只装 `CallExpr`）。
理由：这语言没有分号，`x == 1` 独占一行只能是「把 `=` 写成 `==`」这一类错误；
不报错就会静默接受（§6 有判据）。真需要丢弃值调用请显式写 `discard`（未支持）。

## 4. 覆盖率数字

`rv32im.si`（`--all`）：

| def | 行 | 代码行 | 语句 | 表达式节点 | 覆盖 |
| --- | --- | --- | --- | --- | --- |
| `mode_refresh` | 229–6480 | 3782 | 3604 | 21336 | 100% |
| `mode_run` | 6482–10283 | 1257（+嵌套 15） | 1117 | 5800 | 100% |
| `run_sim`（标签块） | 10287–10368 | 45 | 32 | 92 | 100% |
| `reset_sim` | 189–218 | 22 | 20 | 107 | 100% |
| `set_error` | 170–181 | 9 | 6 | 25 | 100% |
| `get_command`/`get_setting`/`set_setting`/`max`/`halt`/`get_target_cycle` | — | 各 3–4 | 1–2 | 4–13 | 100% |
| `get_ram_value`（**不可达**） | 152–157 | 6 | — | — | **失败@154** |

99 份语料体检（`work/tmp/ast/survey_corpus.py` → `survey.txt`、`survey_all.tsv`）：
**4512 个 def/顶层块，4319 成功（95.7%），193 失败；全语料代码行 121543，成功者 110026 = 90.5%**。
失败的构成（全在 §6 两张清单里）：168 = 模板插值（U-L3）、25 = `get_ram_value` 源里多一个 `)`。

## 5. 保真抽查（AST 片段）

`mode_run:6513`（S 表达式，证据命令见 `work/tmp/ast/sexpr.py`）：

```
(AssignStmt target=(IdentExpr name='speed') op='='
  value=(CallExpr callee=(IdentExpr name='min')
    args=[(CastExpr target=(TypeRef name='U64') expr=(CallExpr callee=(IdentExpr name='get_command')
             args=[(IdentExpr name='ctl_cycle_speed_ms')]))
          (LiteralExpr kind='int' text='10000000000000' value=10000000000000)]))
```

`mode_run:7136`（`U32 (…)` 转换节点带目标类型 + 三目 + `&` 比 `==` 紧）：

```
(VarDeclStmt kw='var' name='vid648' init=(CondExpr
  cond=(BinaryExpr op='==' left=(BinaryExpr op='&'
          left=(CastExpr target=(TypeRef name='U32') expr=(CastExpr target=(TypeRef name='U1')
                  expr=(IdentExpr name='vid646'))) right=(LiteralExpr value=1))
        right=(LiteralExpr value=0))
  then=(CastExpr target=(TypeRef name='U32') expr=(IdentExpr name='vid484'))
  else_=(CastExpr target=(TypeRef name='U32') expr=(IdentExpr name='vid476'))))
```

**不折叠**（`work/tmp/ast/frag/div_shape.si`，照 div_sign 的形状造的两行 + 一个反例）：

| 源 | AST |
| --- | --- |
| `(S32 0 - S32 7) / 2` | `Binary('/', Binary('-', Cast(S32,0), Cast(S32,7)), 2)` ← 两个窄字面量的形状还在 |
| `U8 250 + 10` | `Binary('+', Cast(U8,250), 10)` |
| `S32 (-7) / 2` | `Binary('/', Cast(S32, Unary('-', 7)), 2)` ← 与上一行**结构不同**（div_sign 要的正是这个区别） |
| `1 + 1` | `Binary('+', 1, 1)` |

`mode_run:10253`（`in_scope` 的字符串活下来了）：

```
(PpIfStmt cases=[(IfCase cond=(CallExpr callee=(IdentExpr name='in_scope')
     args=[(LiteralExpr kind='str' text='on_ui_update' quote='"')])
   block=(Block stmts=[(ExprStmt expr=(CallExpr callee=(IdentExpr name='on_ui_update')
     args=[(IdentExpr name='level_input'), (IdentExpr name='level_output')]))] close_line=10257))])
```

## 6. 已支持 / 未支持清单

**已支持（语句）**：`var`/`let`/`const #NAME` 声明、赋值（`=` 与 9 种复合）、调用语句、
`if`/`elif`/`else`、`while`（含 `label:` 前缀）、`switch`+`case`/`default`（不配对花括号）、
`return`（有值/裸）、`break`（含标签）、嵌套 `def`、`#if`/`#elif`/`#else`（`#` 与裸字两种写法）。

**已支持（表达式）**：整数/字符串/模板（无插值）字面量、标识符与 `#常量`、
转换 `T (…)`/`T x`/`T ~x`/`T -8`（带 `@Name`、`Arr[T]`）、一元 `- + ~ !`、
二元 10 级（§2.1）、三目 `?:`、调用（类型实参 `<T,…>` 可出现在实参表任意位置、尾随逗号）、
成员 `.name`、下标 `[i]`、数组字面量 `[…]`、复合字面量 `T {…}`。

**未支持（报错退出，带行号；不是漏）**：

| 构造 | 语料里的量 | 为什么先不做 |
| --- | --- | --- |
| 模板字面量 `` `{expr}` `` 插值 | 168 处 def（`ui_set_text` 那一族） | 插值的语义未定（U-L3）——「先做最小支持」会把范围炸开 |
| `for` / `loop` / `continue` / `when` / `try` / `discard` / 语句层 `type` | 0 处（语料没有） | 出现即报错，不猜 |
| 参数默认值 | 0 处 | 同上 |
| 浮点字面量 | 0 处 | 同上 |
| `'…'` 单引号字面量的**语义** | 258 处字符 | **语法层已收**（`LiteralExpr.quote` 保留定界符），语义等 U-L3 |

**已知边界（会静默接受、后面必须补）**：

- 类型名表是「拼出来的」：语料里被用到的类型都在 `type`/def 头里出现过，漏一个会把 `T (…)` 读成调用
  （不进类型表也可能靠并置规则救回来）。`--stats` 的「警告」区会打出来。
- 名字级作用域不做：闭包变量（`original_start_time`、`speed`）与全局变量在 AST 里长得一样。
- `switch` 的 case 值不做常量求值（`case run` 就是 `IdentExpr('run')`）。
- `#if` 的真假一概不判（`PpIfStmt` 原样留着）。

## 7. 下一步

**接 lowering 的入口**：`expr.py` 已经是「源文件 + def 名 → 语法 AST」，`lower/` 要吃的是
**带类型与作用域的 AST**（冻结接口 §1 的 `TypedProgram`，C++17 侧）。两边的接缝有两个选择，
这是**需要主程/负责人定的**（我没做决定）：

1. 先给 `expr.py` 补一层**最小类型推导**（只推整数位宽/符号性 + 闭包作用域链），
   在 Python 里把 `mode_refresh` 译成一份「形状正确」的 IR 做回归；或
2. 直接把 `expr.py` 当**规格与回归靶子**（`--json` 的输出可以对拍），C++ 侧按 §1 重写一遍。

**lowering 需要 AST 提供的、现在已经有或明确缺的**：

| lowering 要的东西 | 现状 |
| --- | --- |
| 每个节点 `src_line`（报错定位、逐行对拍） | 有 |
| 转换节点的目标类型名 | 有（`CastExpr.target`），位宽/符号从 `[US]\d+` 就能读 |
| div_sign 指示 3 的「源里的形状」 | 有（§5 那四行：`Cast(T,lit)` 的二元运算与 `Cast(S32, Unary(-,lit))` 结构不同） |
| `/`/`%` 的 sdiv/udiv 判据 = **被除数的声明类型** | **缺**：需要类型推导（`var x = S32 0 - S32 7` 推 x 是 S32） |
| `load`/`store` 的对齐（U-L1：`align` 只能是 1） | 不需要 AST 带，builtins 表里写死 |
| 闭包捕获（`halt`/`handle_test_result`/`get_target_cycle`） | **缺**：需要作用域链（谁定义了、谁引用了） |
| `in_scope` 的真假（U-L14，得做两遍/不动点） | **缺**：名字现在可见（`PpIfStmt`），判定是下一步 |
| `#NAME` 编译期常量的值 | 只到 `IdentExpr(name='#X')`；值在顶层 `const` 里，要一个常量表 |
| 位宽提升/截断（U-L7） | **缺**：等 U-L7 结论；AST 这边不做 |

**一个必须先问的语法问题**：`T (…)` 转换与位运算符的紧密度（§2.1 末尾）。
语料里没有能区分的写法，但如果 U-L7 的结论依赖它，就得补一条最小实验再定。

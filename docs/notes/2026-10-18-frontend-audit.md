# 「LLVM 前端」卡点审查（只读）

状态: 有效

日期：2026-10-18 · 审查对象：`simplexc` 的 Simplex 前端，及其上游（卡点）与下游（错误呈现、模块边界）
方式：只读源码 + 跑已存在的 `build/*.exe` + 消融实验；原始材料一律落 `%TEMP%\tc-front\`，仓库除本文件外零改动。

**证据分级**（全文每条结论都标了）：
`[实测]` 本轮跑出来的输出 · `[代码]` 可 grep 到的源码事实 · `[文档]` 仓库文档原文 · `[判定]` 由可复核事实推出的是非判断 · `[推断]` 由前两者推出但未实测 · `[未验证]` 明确没做。

---

## 0. 结论

1. `[实测]` 前端**不是**被自身能力卡住的：它唯一的硬阻塞是 `asm` 语句没被识别，而 98 份语料里真正可达的只涉及 2 个 def，且这两个 def 的 lowering 实现早已另写（`body_memory_clear` / `body_memory_copy`）。
2. `[实测]` 把 `asm` 消融之后，整链仍然 **0/98 通过**，墙移动到 lowering 的两处：`kernel32` 限定名（95 份）、`#SIZE` 常量折叠（3 份）。即「补完前端 asm」的可测收益为 **0 份通过**。
3. `[实测]` 报错文案把排查引离真因：`asm x86_64 { … }` 被读成转型表达式，报成「是不是把 '=' 写成了 '=='？」；同一条兜底还覆盖 `frobnicate x`、裸 `asm` 等。
4. `[代码]` 报错问题的根子是**分类轴缺失**：责任（源码错 / 我们未实现 / 我们 bug）、阶段、可定位性被压成一维，壳的 `catch(...)` 把前端错误统一记成 `InternalError` ⇒ 游戏侧「用户写错」会被统计成「我们的 bug」。
5. `[代码]` 架构不是「糟糕」，是**契约没被执行**：依赖方向单行、函数粒度平坦、粘合层唯一；但 spec 判给 frontend 的类型系统/常量求值/重载解析实际全在 `lower.cc`，且捕获摊平状态机已产出两起真实错序 bug。
6. `[代码]` 用户提出的「设计里没要求报错处理 / 没要求架构」与文档实际情况**相反**：`interfaces` §8 写了错误分类与 span 要求，§0 与主设计 §3.1 写了模块归属。缺的不是设计要求，是**落地与判据**（详见 §2.5、§3.5）。
7. `[判定]` 比「用错语言」更实质的问题是**文档自相矛盾被实现照抄**：设计明文禁止「先在 Python 上做、再移植」（主设计 §8:423），同一份设计又把 Python 原型立为语义权威（`interfaces`:11「当规格读」），代码于是写下 40 处「以 oracle 为准」，其中 `lower/lower.cc:3` 直接写「语义以它为准」。而 `docs/notes/2026-10-18-m5-gap-diagnosis.md:20` 已实测过「Python oracle 不能当 oracle」（§4）。

---

## 1. 卡点本身

### 1.1 三道墙（消融实验）

实验：`prefix = simplexc/corpus/stdlib_prefix.si`，拼接 `prefix + "\n" + src`（照 `work/hook/hook_src_001.si` 的结构），跑 `build/simplexc_cli.exe`。两个变体：
- `base` = 原前缀；
- `noasm` = 把前缀里所有 `asm x86_64/aarch64 { … }` 块按花括号配平整块抹空（等价于「前端认下 asm、但不生成代码」）。

`[实测]`

| 变体 | 结果 | 首错分布 |
|---|---|---|
| base | **0/98 PASS** | 71 份：第 202 行（`memory_clear`）；27 份：第 188 行（`memory_copy`）；报错原文**同一条**：「语句位置出现没有副作用的表达式（CastExpr）——是不是把 '=' 写成了 '=='？」 |
| noasm | **0/98 PASS** | 95 份：`UNSUPPORTED: 未定义的名字 kernel32 @ 第 549 行`；3 份（`098/108/114`）：`UNSUPPORTED: 编译期常量 #SIZE 的初始化式不是常量 @ 第 884 行` |

⇒ 第 188/202 行都是 `stdlib_prefix.si` 的 `asm x86_64 {`；第 549 行是 `kernel32.'Sleep'(…)`；第 884 行是 `const #SIZE = size(type)`。
⇒ 「前端补 asm」把墙从「语法错误」推到「lowering 未支持」，通过份数不变。

### 1.2 asm 缺口的真实规模

`[实测]` 调用次数（`grep -l` 于 `corpus/src/*.si` 与 `corpus/rv32im/rv32im.si`）：

| def | 98 份语料 | rv32im | 体里有什么 | `ast_dump --all` 的 reachable |
|---|---|---|---|---|
| `memory_clear` | 98/98 | 5 | 只有 asm | **是** |
| `memory_copy` | 27/98 | 4 | 只有 asm | 否/是（取决于该份是否调用） |
| `print_using_stack` | 0/98 | 0 | 只有 asm | 否 |
| `breakpoint` | 0/98 | 0 | 只有 asm | 否 |
| `get_cycle_count` | 0/98 | 0 | 只有 asm | 否 |

`[代码]` 不可达 def 不进模块：`lower/module_build.cc:161` `if (fe.live.count(d.name) == 0) continue;`（注释「不可达：不解析、不进模块」）⇒ 后三个 def 的 asm **永远不会被解析**。
`[代码]` 两个可达的 def 在 lowering 侧另有实现：`is_prefix_func` 白名单 `lower/lower.cc:29-31`，`ensure_prefix_fn` 按名字直接发体（`lower.cc:2531-2540`），`body_memory_clear` / `body_memory_copy` 在 `lower.cc:2460` / `:2486` ⇒ 调用点**不会**去 lower 前缀源码里那个 asm 体。

`[推断]` 因此「写一台小汇编器」对 98 份语料的收益为 0；真正被需要的只是「前端遇到 asm 时别误解析」（或等效地，让这两个名字不进解析）。

### 1.3 误导报错链

`[代码]` 前端的语句分流：`parse_statement()` 先匹配已知语句词（`parser.cc:25-31` `stmt_words()`），不匹配再落到 `parse_assign_or_expr()`（`parser.cc:372-393`）；其中 `cast_ahead()`（`parser.cc:564-590`）把 `ident ident` / `ident string` 读成**类型转换**。
`[代码]` `asm` 既不在 `stmt_words()` 也不在 `unsupported_stmt_words()`（`parser.cc:33-37`）⇒ `asm x86_64` 成 `Cast(asm, x86_64)`，`{` 被丢；随后副作用检查抛「是不是把 '=' 写成了 '=='？」（`parser.cc:101-103`）。

`[实测]` 子代理 A 用 22 个自造用例确认同一兜底还覆盖：裸 `asm`、`asm "nop"`、`frobnicate x`、`frobnicate`、语句位置的 `a[0]`；三目里的 asm 会报成「这里要 ':'」。反向对照（分类正确的）：`for` → 「语句 'for' 未支持」、`U99` → 「位宽 U99」、`nosuchfn` → 「未定义的函数 nosuchfn」。

---

## 2. 错误处理现状

### 2.1 诊断链

`[代码]` 每层一套对象、对外五个出口：

| 层 | 对象 | 定义 | 呈现 |
|---|---|---|---|
| 读源 | `ReadError`（普通 struct，**非** `std::exception`） | `frontend/frontend.hh:30` | CLI `读不了 %s：%s` rc=2（`cli/simplexc_cli.cc:288-290`） |
| 词法 | `LexError{line,col,msg}` | `frontend/lexer.hh:27` | `词法错误：第 L 行 第 C 列` rc=1 |
| 语法 | `ParseError{line,col,msg}` | `frontend/parser.hh:28` | `语法错误：…` rc=1（`simplexc_cli.cc:285`） |
| 声明扫描 | 无异常，`ScanResult{errors,unsupported}` | `frontend/decls.hh:52-57` | `errors` → `CompileFail("声明层错误：…")`；`unsupported` **无读点** |
| lowering | `Unsupported{msg,line}` / `CompileFail{msg}` | `lower/lower.hh:42` / `:52` | `UNSUPPORTED: … @ 第 N 行` / `编译失败：…` rc=1 |
| 装配 | `AsmStatus` + `detail`（无源码位置） | `assemble/assemble.hh:48-55` | `装配失败：status=d %s` rc=1 |
| 壳 C 边界 | 5 个分类串 | `shell/simplexc_core.h:40-45` | **只有类别名过边界**，文本走 `std::cerr`（游戏进程无接收者） |
| 转发层 | `stage=` / `kind=` | `tools/compile_hook.c:809-818` | hook.log `error` 行 |

`[代码]` 冻结版设计的 `CompileError{ErrKind,message,Span}`（`frontend/ast.hh:27-37`）是**死代码**：全仓只有定义本身。

### 2.2 类别混同（可核对的后果）

`[代码]` 前端只有一个出口：`for` 未支持（`parser.cc:343`）与真语法错用同一个 `ParseError`。
`[代码]` 壳把所有前端错误归 `InternalError`：`shell/simplexc_core.cc:185-189`（`catch(...)`）。
`[推断]` 因此游戏侧 manifest/hook.log 里「用户源码写错」与「我们未实现/我们 bug」不可区分，回退率的分子分母都不可解释。

`[实测]` 98 份语料对这套分类**没有信息量**：首错 distinct 只有 2 条/变体（§1.1），全部死在两个前置阻塞点。

### 2.3 静默失败（rc=0，仍产出 blob）

`[实测]` 子代理 A 的用例：

| 源码 | 结果 | 机制 |
|---|---|---|
| 顶层未知行 `this is not simplex at all` | rc=0，760 B blob | `top_unsupported` 只写不读（写点 `frontend/frontend.cc:305`，字段 `frontend/frontend.hh:112`） |
| 不可达 def 里的 `asm` / `var = = =` | rc=0 | `lower/module_build.cc:165` 整段跳过，连解析都不做 |
| 未识别语句关键字 | rc=0（被当表达式丢掉） | §1.3 的兜底 |
| 解析期 `notes` | CLI/壳都不看 | 只进 `ast_dump` 统计（`ast_dump_main.cc:209`） |
| `compile_real.dll` 加载失败 | 不写 manifest / error / 失败 Result | `compile_hook.c:777` `return NULL`（src dump 在 `:774`，所以捕获目录有 `.si` 而 manifest 缺行） |
| `ours_fail_result` 的 `malloc` 失败 | 什么都不写就返回 | `compile_hook.c:321-323` |

### 2.4 位置信息质量

`[代码]` `err_line()` 一律 `col=0`（`parser.cc:103`），尽管 token 保留真列号；`Unsupported` 只有行（`lower.hh:42-49`），`CompileFail` 没有位置（`lower.hh:52`）；装配错误没有源码位置。
`[代码]` 6 处 `Unsupported(..., 0)` 会报「第 0 行」：`lower.cc:375,383,431,440,569,720`（`[未验证]` 未构造出触发用例）。
`[实测]` 文本重复一次：`读不了 X：读不了 X：No such file…`（`frontend.cc:320` 自带前缀 + `simplexc_cli.cc:289` 再加一次）。

### 2.5 与 `interfaces` §8 的差距

`[文档]` §8 要求：错误可定位到源码 span；`CompileError.kind` 与 §5 记录字段一一对应且不另处定义；禁止静默降级。
`[代码]` 已落地：5 名分类串唯一一处定义（`simplexc_core.cc:45-51`）、§5 `impl` 四值词表与 manifest 列、每次调用都写、熔断/超时各带 stage、VEH 只包 ours 段。
`[代码]` 未落地：span 过 C 边界即丢（只剩类别名）、分类实际定义在 `simplexc_core.cc` 与 `compile_hook.c:146-148` **两处**、`SymbolUndefined`/`RelocUnsupported` 的符号名与重定位类型只进 stderr、前端错误在壳里全部折成 `InternalError`。
`[推断]` 所以「设计没要求报错处理」不成立；断的是「规格 → 实现 → 判据」这条链，其中判据（manifest 列）只覆盖到类别名这一层。

---

## 3. 架构现状

### 3.1 规模与依赖

`[代码]` `simplexc/` 53 文件、24388 行（代码 20202，占 82.8%）；其中 C++ 11952 行、Python 12436 行（Python 大头在 `tools/`）。

| 目录 | 文件 | 代码行 |
|---|---|---|
| lower | 8 | 6396（C++ 5253 / Python 2344） |
| frontend | 16 | 5310（C++ 4542 / Python 1779） |
| tools | 12 | 4799（全 Python） |
| shell / cli / assemble / runtime | 13 | 1562（全 C++） |

`[代码]` 单文件最大：`lower/lower.cc` 3431 行（= 产品 C++ 的 28.7%）、`lower/ir_build.py` 2344、`frontend/expr.py` 1173。
`[代码]` 最大函数：`frontend/parser.cc:292 parse_block` 591 行/105 分支；其次 `assemble.cc:196 assemble` 306 行。`lower.cc` 反而是最平的：166 个函数、最大 125 行、均值 19 行。
`[代码]` 依赖方向单行：全仓仅 7 条跨目录 `#include`，无反向（frontend 不 include lower，assemble 不 include lower）。

### 3.2 双实现

`[代码]` 两套前端（Python `decls.py` 606 + `expr.py` 1173 vs C++ 4542）、两套 lowering（`ir_build.py` 2344 vs `lower.cc` 3431）。
`[代码]` Python 栈无活引用：`tools/build.py` 只编 `.cc`；`tools/check.py` 5 步无一步调它；`grep -rn ir_build --include=*.py` 只命中自身 docstring 与 `tools/m5/coverage.py:120` 的一句口径注释。
`[代码]` 粘合层**没有**重复：唯一实现 `lower/module_build.cc:157 build_module`，三个调用者 `simplexc_cli.cc:173` / `simplexc_core.cc:152` / `core_selftest.cc:82`。
`[代码]` AST 遍历有三份：`lower.cc:73 for_each_child`、`ast_dump.cc:355 render_tree`、`ast_dump.cc:106 node_to_json`。解析路径没有重复（`ast_dump_main.cc:524` 复用同一个 `Frontend`）。
`[代码]` 3 个 spike 夹具仍在产品构建清单 `tools/build.py:294` 的 `ALL`：`cli/llvm_prog.cc`（自称「前端接进来以后本文件就退化成测试夹具」，`llvm_prog.cc:5-7`）、`cli/prog_empty.cc`、`shell/llvm_link_probe.cc`。

### 3.3 状态与所有权

`[代码]` `Lower` 有 37 个数据成员（`lower/lower.hh:389-470`）；「当前函数」状态 `FnState` 10 项（`lower.hh:201-212`），`save_state`/`restore_state` 逐字段赋值各 10 行（`lower.cc:573-600`），共 **7 对调用点**（`lower.cc:2531/2540`、`2589/2616`、`2639/2664`、`2996/3026`、`3040/3078`、`3092/3149`、`3404/3414`）。
`[代码]` 加一个成员要改 3 处（`FnState` / 两个函数），编译器与判据都不查。
`[文档]` 这套设计已产出真实错序 bug：`5d4ff13`「模板里同一 def 的多次调用共用最后一段的结果」、`ebc97c2`「嵌套 def 的捕获按调用点摊平」。
`[代码]` 兜底是硬阈值：`lower.cc:2567` `instances >= 64` 抛 `Unsupported`，注释自述名字集合的幂集可能无界。
`[代码]` 全仓 **0 个 `assert(`**，也没有单测框架（`gtest`/`catch2`/`doctest` 均无命中）。

### 3.4 判据覆盖

`[代码]` 判据入口：`tools/check.py`（5 步：corpus / host_selftest / lower_equiv / hook_offline / probe_006）、`build/lower_selftest.exe`（908 行，L2 手写 AST→COFF、L3 装配自校验、L4 四个语义档案最小 AST）、`build/core_selftest.exe`、`tools/m5/coverage.py`、`tools/host/selftest.py`。
`[代码]` `lower` 的 166 个方法里，只有 4 个有最小 AST 用例（U-L1/U-L7/U-L8/除法）；其余靠 98 份语料间接测。
`[代码]` 前端没有判据步骤：`build/ast_dump.exe` 由 `tools/build.py:228` 构建，但 `check.py` 5 步里没有它。
`[代码]` 主设计 §7.5 的三道门只落地 1 道：依赖门（`objdump -p … | grep -i python`）与延迟门（`177.si` ≤4 s）在 `check.py` 里无实现。
`[未验证]` §7.5 原文的具体措辞未逐字核对（本轮按子代理 B 的转述）。

### 3.5 与 spec 的偏差

`[代码]` `prompt2.md` 列的「6 处不一致」逐条核到代码，**6 处全部已对齐**：`assemble()` 签名 `ExternResolver&`、`AsmStatus` 5 值、桩 12 字节、`impl` 4 值词表、`reference` 也记 `impl`、宿主表外 movabs 硬错误。
`[文档]` 仍在的真偏差：
1. **职责错位**：`interfaces` §0（`:36-40`）与主设计 §3.1（`:203`）把类型系统 / 常量求值 / 重载解析 / `#if in_scope` / 内建识别判给 frontend；实际前端只出无类型 AST（`lower/lower.hh:236` 自证「解析在 lowering 侧」），这些都在 `lower.cc`（`static_type:1716`、`resolve_inst:1903`、`cv:2678`、`pick_def`、`unify`、`compatible`、`in_scope_const`）。
2. 主设计 `:203,204` 的组件表仍写 frontend/lowering「C++17 版待写」（已写完）。

---

## 4. Python → C++：设计判这条路禁止，代码却把它的产物供为语义权威

### 4.1 时序 `[代码]`（禁令是事后补的）

| 时间（+0800） | 提交 | 事 |
|---|---|---|
| 10-17 18:26 | `c5ed5e0` | Python 词法 + 顶层声明表 |
| 10-17 19:27 | `1a6ab4f` | Python 语句/表达式 AST |
| 10-17 19:58 | `cba3adb` | Python lowering v0（006 逐 tick 全等） |
| 10-17 22:52 | `00c7961` | **设计改口**：前端定为 C++17 + M4.5，同一提交里写入 §8「禁止『先在 Python 上做、再移植到 C++』」 |
| 10-17 23:31 | `88479f0` | Python 的 17 条 lint 清零（禁令之后仍在动 Python，属允许的「缺陷」例外） |
| 10-18 01:37→02:15 | `bf3f7fa`…`b861e60` | C++ 前端 + lowering + CLI 全部落地 |

`[代码]` 设计里**没有**「当初选 Python 是错的」这样一句，但有等效判定（§8:423 的禁令 + §3.1:213-216 的语言分工理由：延迟预算 / drop-in 部署 / 崩溃面）。两点需如实记下：①它是**事后 3 小时**补的（禁令落笔时 Python 前端与 lowering 已跑通）；②规模上是把前端 + lowering 写了两次（Python 4123 行 → C++ 7973 行）。

### 4.2 文档一边禁止、一边授权 `[文档]`

同一份设计同时给了互相打架的指令：

| 指令 | 出处 |
|---|---|
| **禁止**这条做法 | 主设计 §8 `:423`「**禁止「先在 Python 上做、再移植到 C++」**」；`interfaces` 1.2 `:13-14` 同句 |
| **豁免**已经做了的这一次 | `interfaces` `:14`「M4.5 是一次性收尾，不构成先例」；主设计 §9 `:462-465` 把「前端换 C++」列为里程碑 |
| 把它的产物立为**语义权威** | `interfaces` `:11`「Python 原型**冻结为参考实现（oracle）**：只读，用途 = 语义查询（**当规格读**）、差分对照、延迟基线」；§0 `:41` 同义；主设计 §3.1 `:203,204` 把「现有 Python 原型（冻结为 oracle）」写在「**C++17 版待写**」旁边 |
| 谁说了算 | `interfaces` `:14` 只规定「以下**各节**（= 接口）与 Python 原型不一致时以本文件为准」——**语义**没有收回，仍指向 Python |

`[判定]` 这是设计层的模糊，不是执行者单方面的误解：文字上「这条路禁止」与「它的产物当规格读」并存，且**没有任何一句宣告 oracle 制度随 M4.5 完成而终止**。产品侧文档同样停在旧说法：`frontend/README.md:7`「产品实现 = C++17（**待写**，M4.5）」，主设计 `:203,204` 同。

### 4.3 代码里把错误行为写成了规范 `[代码]`

- `lower/lower.cc:3`：「行号引用形如 `oracle:1234`，指冻结原型 `simplexc/lower/ir_build.py` 的那一行 —— **语义以它为准**」。
- `lower/lower.hh:3-13`：小节标题就是「**为什么逐条复刻 oracle 而不是「按 LLVM 的自然行为写」**」，并写「oracle 的行为是实测过的参考实现语义……改动前先读那几行」。
- `lower/lower.hh:1`、`module_build.cc:3`、`module_build.hh:6,11`、`lower_selftest.cc:7`（「每条断言都能对着 `ir_build.py` 的对应行检查」）同义。
- 前端 7 个文件各一行「语义 oracle = 冻结原型 `expr.py`/`decls.py`」：`ast.hh:4`、`ast_dump.hh:3`、`ast_dump_main.cc:3`、`decls.hh:3`、`frontend.hh:3`、`lexer.hh:3`、`parser.hh:3`。
- 合计 `[实测]`（`grep -rn "oracle\|ir_build\|expr\.py\|decls\.py" simplexc/ --include=*.cc --include=*.hh`）**40 处、13 个文件**。

`[判定]` 「语义以 `ir_build.py` 为准」是**错误行为**，三条依据都可复核：

1. 与设计自己的规定冲突：`interfaces` §0「语言分工（唯一权威）」+「产品路径不得依赖 Python」，而 `:14` 的兜底只覆盖接口；主设计 §8 `:423` 明文禁止这条路径。
2. 与仓库内的实测反证冲突：`docs/notes/2026-10-18-m5-gap-diagnosis.md:20` 的小节标题就是「**Python oracle 的行为：同样报错，不能当 oracle**」——那一轮 49 份 `{…}` 插值缺口上，oracle 的结论与参考编译器相反。
3. 它不可自愈：Python 栈在 `88479f0` 之后**零提交**，C++ 侧此后 30+ 个语义提交；而唯一的例外触发器「跑不起来 / 结果不可复现」（`interfaces` `:12`）**永远不会响** —— `[实测]` `python simplexc/lower/ir_build.py --src simplexc/corpus/rv32im/rv32im.si --list-unsupported` 仍 rc=0、「UNSUPPORTED 0 条」。

### 4.4 账本与规模 `[代码]`

- 拆分前的 `docs/facts.md` 有 **12 行**提到 `ir_build.py`，其中 **4 行**是结论的「验证：」命令（原行号 `:262`、`:272`、`:283`、`:295`）——即这些结论的证据源是死代码。
- `facts/semantics.md` 记着原型把 `CodeGenOptLevel` 写成 0 造成的 -O0 度量返工（另见 `pitfalls.md:95`、`facts/corpus-host.md`）。
- 遗留规模：Python 4123 行 = 全树 16.9%，仍在产品树内，被 13 个 C++ 文件、`interfaces`/主设计/`frontend/README.md`、`facts/` 共同引用。

`[未验证]` 「迁移吃掉了多少 agent 的有效产出」无法从仓库度量，本报告不给工时结论。`[未验证]` 也没有对照实验能证明「一开始就写 C++ 会更快」——那是反事实，不做判断。

---

## 5. 不确定 / 推测 / 未验证（汇总）

- `[推断]` LLVM fatal error（`report_fatal_error` → abort，码 `0xC0000409`）会穿透壳：依据是「全仓无 `LLVMInstallFatalErrorHandler`」+「VEH 白名单不含该码」（`compile_hook.c:246-264`）。**未构造实测**。
- `[未验证]` 参考实现对畸形源的态度：顶层未知行、不可达 def 里的语法错、未识别关键字、`asm` 变体——参考是静默吃、报错、还是 abort，本轮没测。这决定「哪些必须显式失败」的规格内容。
- `[未验证]` `Timeout` / `OursFault` / VEH 熔断的实跑路径（`simplexc/tools/hook_offline.py` 有 `ours_timeout`/`ours-nosrc` 用例，未跑）。
- `[未验证]` `compile_real.dll` 缺失时游戏侧的实际表现。
- `[未验证]` 6 处 `Unsupported(...,0)` 能否被真实源码触发。
- `[未验证]` 游戏引擎是否真的读取 `Result.error` 文本（决定错误文本过边界的必要性）。
- `[推断]` 子代理 B 的「类型/常量逻辑约 300+ 行」是估算；`static_type`/`resolve_inst`/`cv` 的实际可搬迁边界未逐函数核。
- `[推断]` 「捕获摊平方案在语义上无界 ⇒ 只能重写」是审查判断（依据是 `lower.cc:2565-2570` 的自述注释 + 两起错序 bug），不是实测。
- `[未验证]` 98 份语料在错误分类维度的细分分布：当前全被两个前置阻塞点截断，无信息量。

---

## 6. 复现

```bash
# 三道墙（98 份语料 × 2 变体，约 3 分钟；输出落 %TEMP%\tc-front）
python %TEMP%\tc-front\probe1.py

# 单个 def 的 AST 与可达性
cd tc-toolkit
./build/ast_dump.exe --src simplexc/corpus/stdlib_prefix.si --def memory_clear
./build/ast_dump.exe --src "<prefix+语料拼接源>" --all      # 看 reachable 列
```

原始材料：`%TEMP%\tc-front\{probe1-base.tsv, probe1-noasm.tsv, probe1.py, subA\, subB\}`（不进 git）。
子调查全文：`%TEMP%\tc-front\subA\report.md`（错误处理，117 行）、`%TEMP%\tc-front\subB\report.md`（架构，140 行）。

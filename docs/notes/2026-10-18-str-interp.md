# 模板插值 ① + Str 拼接：语义实测与实现（M5 第一块）

实现者（`simplexc/frontend/**` + `simplexc/lower/**` 的唯一写者）。器材与原始日志：`work/tmp/m5interp/`。

## 一句话

`{…}` 插值的渲染语义**全部实测**（不是从 Python 原型推的）：`Int`/`S*` 有符号十进制、`U*` 无符号十进制、
`Bool` 是 `true`/`false`（**`U1` 不是 Bool**）、`Str` 原样、`Ptr` 是地址的十进制；`+` 拼接 = 长度相加 + 逐字节。
**枚举插值参考编译器自己崩**（见下）→ 语料里不存在这种形状。

## 器材（可复用）

驱动口径照抄 `docs/notes/2026-10-18-m5-gap-diagnosis.md` §①-2，脚本落在 `work/tmp/m5interp/`：

| 脚本 | 干什么 |
| --- | --- |
| `probe.py` | 生成片段（`work/tmp/ul7x/base.si` 452 行 + 探针 def + 顶层调用）→ 参考编译 → 宿主 2 tick → 读 `ui_buffer` 页的 `+offset`（u64 长度）与 `+offset+8`（字节） |
| `referr.py` | 参考编译的**报错文本**（`tcdll.py` 只打结构，不打消息） |

一条探针 = 一次参考编译 + 一次宿主运行（约 15 s）。`python work/tmp/m5interp/probe.py [名字…]`。

## 实测结果（渲染语义）

`set_text(text, offset)` 先写长度再逐字节写字符，所以「读出的字节」就是模板求值结果。

| 探针 | 模板 | 读出 |
| --- | --- | --- |
| `unsigned` | `` `{U8 200}` `` / `` `{U16 60000}` `` / `` `{U32 4000000000}` `` / `` `{U5 31}` `` / `` `{U64 max}` `` | `200` / `60000` / `4000000000` / `31` / `18446744073709551615` —— 窄无符号按**无符号值**渲染，不做符号扩展 |
| `signed` | `` `{S8 -128}` `` / `` `{S16 -32768}` `` / `` `{S32 -2000000000}` `` / `` `{S64 min}` `` | `-128` / `-32768` / `-2000000000` / `-9223372036854775808` —— 按**声明位宽**符号扩展后取十进制；S64 最小值也对（幅度按无符号取负算） |
| `bool` | `` `{Bool true}` `` / `` `{Bool false}` `` / `` `{U1 1}` `` / `` `{U1 0}` `` / `` `{1 < 2}` `` / `` `{U1 (1 == 1)}` `` | `true` / `false` / `1` / `0` / `true` / `1` —— **Bool 与 U1 是名义上不同的类型**（同一台机器上两种渲染） |
| `runtime` | `store` 进内存再 `load(<U8>/<S8>/<U16>)` 后插值 | `200` / `-128` / `60000` —— 运行期值同样按**声明类型**渲染 |
| `other` | `` `{Ptr 2416633380864}` `` | `2416633380864`（地址的无符号十进制） |
| `other` | `` `{ETest bb}` ``（enum） | **参考编译器 fatal**：`front_end.nim(2465, 5) 'inner_string.info.value != EXP_NULL'` —— 枚举插值是参考自身的洞，语料里不可能有 |
| `concat` | `"ab" + "cd"` / `` `A{"hi"}` + "!" `` / `"x" + `{"hi"}y` `` | `abcd`(4) / `Ahi!`(4) / `xhiy`(4) |

其它已在 `m5-gap-diagnosis.md` 测过、本次直接沿用：`Int` 有符号十进制、`Bool`/`U1` 之别、`Str` 原样、
多段按出现顺序、负号、结果落在**线程栈**、长度显式（`p[-8] >> 16`）、`extern` 为空（产物内联）。

**`I8` / `I32` 不是类型**（`referr.py` 报 `Compile error`）：只有 `^[US]\d+$` 这一种位宽拼写。

## ② 的遗留实验：`return <expr>)` 后面还跟 token

`work/tmp/m5interp/retcases/`（同样口径：参考编译 + 宿主读 `set_text` 的字节）：

| 片段 | 参考 | 读出 |
| --- | --- | --- |
| `def f(a: Int) Int { return a }` | 成功 | `{f(1)}` → `1` |
| `def f(a: Int) Int { return a) }` | 成功 | `1` |
| `def f(a: Int) Int { return a) + 1 }` | 成功 | **`1`**（`+ 1` 被丢掉，不是 `2`） |
| `def f(a: Int) Int { return a)` + 换行 + `store(...)` `}` | 成功 | `1`（后面那条语句也**没有生效**） |
| `return a) ; store(...)` | **fatal** | `tokenizer.nim(281, 11) 'false' tok_unknown` —— `;` 根本不是 token，与括号无关 |

→ 参考对「`return` 表达式后多余的 `)`」的容忍是**静默吃掉它以及后面的一切**（对产物零影响），
不是「只吃掉一个 `)` 再接着解析」。我们的窄修（`d5c9e82`）只吃掉一个 `)`，之后**会**继续解析
（`return a) + 1` 在我们这边是「表达式语句 `+1` → 没有副作用」这类错）。语料里没有这种形状，
所以不影响 49/17 份；这条差异记在「遗留风险」里。

## 实现

三次提交：阶段 1 前端 = `f2078f2`、阶段 2 lowering = `9b00169`、缓冲 alloca 位置修正 = `2b85f54`。

### 前端（`f2078f2`）：解析成 `TemplateExpr`

词法器**不拆**插值（模板 token 的 `text` 是反引号之间的原文，只带一个 `interp` 标记），所以拆分在
`Parser::parse_template` 里做：

1. 按 `{` 切文字段；`}` 用**嵌套计数**配对（引号里的花括号不算，走 `scan_quoted_at`）—— 没配上就报
   「`'{'` 没有配对的 `'}'`」，列号指向那个 `{`。
2. `{…}` 里的代码交给一个**子 `Parser`**（词法器原样复用：`tokenize(片段)`），然后把子 token 的列号
   平移到源位置（`源列 = 模板起始列 + 1 + 片段前的码点数`）。模板不可能跨行（`scan_quoted` 遇换行即
   未闭合），所以只需要平移列号。子解析完后不是 eof 就报「模板插值里多余的 …」。
3. 无插值的模板**不走这条路**：仍是 `LiteralExpr{lit_kind="template"}`（那条路径的产物已验证）。

`scan_quoted_at` / `code_points_between` 从 `lexer.cc` 的匿名 namespace 提到 `lexer.hh`（同一份实现，
不是抄一份）。

### lowering：值渲染 + 拼接

| 函数 | 干什么 | 为什么这么切 |
| --- | --- | --- |
| `template_expr` | 各插值段**先各求值一次** → 按最坏尺寸在栈上开一块缓冲 → 顺序写入 → 按**实际游标**回填 `p[-8] = len << 16` | 两趟式的「先算长度再写」需要预先知道十进制位数；改成「按最坏尺寸开、写完用游标算长度」就不用那个循环 |
| `part_capacity` | 一段值的**最坏字节数**：`Bool` = 5；其它整数/`Ptr` = 21（20 位 + `'-'`）；`Str` 不算在这里（返回 0，调用方单独加 `len_of`） | 尺寸只是个**上界**，真正的长度按写完后的游标算 → 不需要预先知道十进制位数 |
| `render_part` | 按**静态类型**选渲染：`Str` 拷贝、`Bool` 写 `true`/`false`、整数/`Ptr` 走十进制 | 语义是「按声明类型渲染」（实测：同一台机器上 `U1 1`→`1`、`Bool true`→`true`），所以类型必须留着，不能在 lowering 里退化成裸 i64 |
| `write_literal` | 文字段逐字节写常量 | 内容编译期就定死，不需要拷贝循环 |
| `str_concat` | `+`（`Str`,`Str`）：新串 = 左 + 右，尺寸 = 两边 `len` 之和 | 与模板共用「栈缓冲 + 头 word + 显式长度」这一套 |
| `len_of` | `u64(p-8) >> 16` | `.len()` 与插值里的 `Str` 段**共用一处**（原来只在 `call_method` 里） |
| `ensure_dec_fn` | `__simplexc_dec(p, 幅度, 负号)`：19 次比较定长度、从末尾往前填十进制 | 参考的十进制转换是**产物内联**代码（`extern` 为空），所以不能引 `sprintf`/`memcpy` 之类的外部符号 |
| `ensure_copy_fn` | `__simplexc_copy(dst, src, n)`：逐字节循环 | 同上；`LLVMBuildMemCpy` 的动态长度在 x86 后端会落成 `memcpy` 调用 → 未解析符号（装配器要么造桩、要么宿主 `extern` 非空） |

两个辅助函数是 **internal linkage**（`LLVMSetLinkage(…, LLVMInternalLinkage)`），按需生成、整个模块一份
（`helpers_`），与 `ensure_prefix_fn` 的 `save_state`/`begin_function`/`restore_state` 同一套写法。

### 缓冲的 alloca 位置（这是第二个提交修的那个崩）

**没有 `Str` 段时尺寸是编译期常量** → alloca 放进 **entry 块**（`alloca_in_entry`）→ LLVM 当它
**静态 alloca**（帧内一个固定槽）。含 `Str` 段时尺寸是运行期的，只能在当前位置开**动态** alloca。

为什么这很重要（实测）：缓冲原来是当前位置（run_sim 循环体里）的动态 alloca，LLVM 于是生成

```asm
mov    $0x20,%eax
call   ___chkstk_ms      ; 壳自带（shell/entry_shim.S）
sub    %rax,%rsp
```

而我们的 `___chkstk_ms` 内部写 **%r10**（`lea 0x18(%rsp),%r10`，与 libgcc 的实现同形），LLVM 却把 %r10
当「跨这次 call 仍然有效」用（`movabs $ui_buffer+20000,%r10` 之后跨那次 call 接着用它算
`0xbb8(%r10)`）→ `rsp` 变垃圾 → 写崩在产物 `+0x730`（`signed_loop` 探针，写地址 `0xADDA60478`）。

换成静态 alloca 后那次 call 不再出现（也顺手去了「循环里每轮涨栈」的隐患）。**含 Str 段的模板与
`str_concat` 仍走动态 alloca**（尺寸真是运行期的），那几个探针实测不崩（见下），但 %r10 这个隐患
在那条路径上仍然潜在 —— 见「遗留风险」。

符号性按**声明位宽**走：有符号先 `sext64` 再取十进制，幅度用**无符号取负**（`0 - x`）—— 这样
`S64` 最小值也对（实测参考就是 `-9223372036854775808`）。`Bool` 用 `is_bool` 这个**名义标记**区分
`U1`（`ty_bool()` 与 `make_int(1,false)` 在算术语义上等价，但渲染不同 —— 接口 §1 的「Bool ≡ SizedInt{1,unsigned}」
管的是算术，不管渲染）。

## 验证

器材：`work/tmp/strinterp/`（coverage 报告 / 原始日志 / 探针页快照）。三次提交：`f2078f2`（前端）、
`9b00169`（lowering）、`2b85f54`（alloca 位置修正）。

### 1. 渲染语义：探针逐字节与参考相同（最强的那条）

把探针调用放进 `run_sim` 体（顶层语句我们的产物会丢，见「第二类缺口」），两边都执行，
再读 `ui_buffer + offset` 的 `(len, bytes)` 对比（`python work/tmp/m5interp/cmp.py <探针>_loop.si`）：

| 探针 | 对照的模板 | 结果 |
| --- | --- | --- |
| `unsigned_loop` | `U8 200` · `U16 60000` · `U32 4000000000` · `U5 31` · `U64 max` | 5/5 **逐字节相同** |
| `signed_loop` | `S8 -128` · `S32 -2000000000` · `S64 min` · `S16 -32768` | 5/5 相同 |
| `bool_loop` | `Bool true` · `Bool false` · `U1 1` · `U1 0` · `1 < 2` · `U1 (1 == 1)` | 6/6 相同 |
| `ptrstr_loop` | `Ptr 2416633380864` · `` `s{Str}` `` · `` `{Str}{Str}` ``（后两条走**动态** alloca） | 3/3 相同 |
| `runtime_loop` | `store` 进内存再 `load(<U8>/<S8>/<U16>)` 后插值 | 3/3 相同 |
| `concat_loop` | `"ab" + "cd"` · `` `A{Str}` + "!" `` · `"x" + `{Str}y`` | 3/3 相同 |

（改 alloca 位置之前，`signed_loop` 在产物 `+0x730` 写崩 —— 这就是上面那个 chkstk/%r10 的故事。）

### 2. 那 49 份：ParseError 归零、可装配数上升（`work/tmp/strinterp/coverage.md`）

```
汇总：总 49 · 可装配 38 · 逐 tick 全等 0 · cli_fail 11 · host_fail 17 · diff 21
      装配回退率 22.4%（改前 100%）· 差分回退率 100.0%
```

- **ParseError 归零**：`插值` 类错误从 49 → **0**（这是本轮的硬判据）。
- **可装配 0 → 38**。剩下的 11 份 cli_fail 全是**另一类**缺口（不是插值）：
  `ui_set_hidden()` 重载解析 5 · 未定义函数 `array` 3 · `switch` 主体是 `Str` 2 · 未定义函数 `random` 1。
- **逐 tick 全等 0**：两类原因，**都不是插值带来的**（下面分别给证据）。

#### 2a. 21 份 `diff` = 基线就有的「段哈希」类

`diff` 全为同一形状：`tick 1: 段 0x232a6ff0000 哈希不同`（`A`=参考、`B`=我们；我们的值恒为
`b27af0b0…`）。拿**改前就存在的 17 份同类语料**做对照（`work/tmp/strinterp/cov_baseline17.tsv`）：

- 那 17 份现在仍是 17 份 `diff`、仍是 9~10 条差异、**同一形状**；
- 005 的参考侧哈希 `2517ab16f87a203c…` 与仓库里改前的 98 份报告**逐字相同**（`docs/notes/2026-10-18-m5-coverage.md:102`）。

→ 这一类是**先前就有的**（M5 基线的 17/29），我的改动没碰它。

#### 2b. 17 份 `host_fail` = **参考产物**在宿主里 AV

`ref: [host] 异常 code=0xC0000005 … params=[1, …]`，且偏移每次跑不一样（试 5 次都失败）。
同一批 id 在不同轮次里 10~23 份失败（间歇），与 034/090 同类 —— **判不了等价**，本报告不把它们算进结论。
id：001、044、047、050、051、053、060、061、080、088、094、122、126、128、129、133、137。

### 3. 不退化：改前 equal 的 9 份仍然 0 差异（`cov_regress.tsv`）

```
[9/9] 002/004/006/020/026/038/058/176/177 全部 equal · diff=0
```

### 4. 判据入口：`check.py` 5 PASS（`check2.json`）

```
corpus PASS · host_selftest PASS · lower_equiv PASS（rv32im 26340 B · entry 0x63A0 · 10/10 tick · diff_count=0）
· hook_offline PASS（8/8）· probe_006 PASS —— 5 PASS · 0 FAIL · 0 SKIP
```

### 5. 延迟门：`corpus/src/177.si` 端到端 **0.31 s**（≤ 4 s）

`177` 0.312 s（blob 37364 B / entry 0x8E90，与改前逐字相同）· `001` 0.240 s · `051` 0.103 s。

### 6. pi-lens 命中 0

入口照 `docs/notes/2026-10-18-ir-build-lint.md`（ast-grep + pi-lens 全部 298 条规则，
`work/tmp/strinterp/lint/sgconfig.yml`）：本轮改过的 9 个文件（`lower/{lower.cc,lower.hh}` +
`frontend/{parser.cc,lexer.cc,ast.hh,ast_dump.cc,frontend.cc,lexer.hh,parser.hh}`）**0 条**。
入口自证：故意写 `printf("hi")` 的探针文件能命中 `cpp-no-printf`。

## 第二类缺口（**与插值无关**，本轮只记录、没改）

1. **顶层非声明语句被丢掉**：`lower/module_build.cc` 只收 `type`/`def`/`label`/`var`/`const`，
   所以顶层写 `exp()` 或 `store(...)` 的产物里**什么都没有**（实测：`rawstore.si` 顶层 `store` 参考写页、
   我们不写；把同一句放进 `run_sim` 体就两边都写）。campaign 摸底卡顶层（`docs/notes/2026-10-18-campaign-survey.md`）
   大概就是这个。
2. **`switch` 的主体是 `Str` 时产出非法 IR**：`icmp eq ptr %ld, i64 0`（`s_switch` 把 `Str` 主体
   与整数常量直接比）→ `IR 校验失败`，命中 024、151 两份。
3. 另三类**原先就有**的（`ui_set_hidden()` 重载解析、`array`、`random`），本轮没碰。

## 遗留风险

1. **动态 alloca 路径上的 %r10 隐患**：含 `Str` 段的模板与 `str_concat` 仍会拉出 `___chkstk_ms`。
   探针实测不崩（`ptrstr_loop`/`concat_loop` 全对），但那是因为那几处的代码没有跨 chkstk 用 %r10。
   真正的修法在壳（`shell/entry_shim.S` 的 `___chkstk_ms` 要不要保 %r10/%r11）或改用别的缓冲策略
   —— 都不在本轮允许改的目录里，**没动**。
2. **`return <expr>)` 后面还跟 token**：参考静默吃掉后面的一切（`return a) + 1` 读出 `1`），
   我们的窄修只吃一个 `)` 然后继续解析。语料里没这种形状，**没改** `parser.cc`。
3. **001 这类大文件的差分判不了**（参考产物在宿主里 AV），所以「001 与参考 10 tick 全等」这条
   本轮**没做到**，换成上面的探针逐字节对照 + 21 份同类差异的基线对照。
4. **枚举插值**没有语义可对照（参考自己崩）→ 我们报 `Unsupported`；若将来语料出现这种形状，
   得先定语义。

### 没做（有意）

- **枚举插值**：参考编译器自己 `front_end.nim(2465)` fatal → 没有可对照的语义，我们这边直接
  `Unsupported`（语料里也没有这种形状）。
- **转义**：文字段里有 `\` 就 `Unsupported`（U-L10 未定，与 `string_literal` 同口径）。
- **顶层语句**：`{count} / 3` 这种插值在顶层语句里能编译，但**顶层非声明语句我们整体丢掉**（见下节
  的独立缺口）。

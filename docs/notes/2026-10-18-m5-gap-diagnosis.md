# M5 两个最大缺口的诊断：①模板插值 ②意外的 ')'

诊断者（read-only 产品代码）。器材与原始日志：`work/tmp/m5gap/`（最小片段、参考产物、host 页快照）。
口径：所有「参考」结论都来自**参考编译器本体**（`compile_real.dll`，`work/tmp/m5gap/tcdll.py` =
`tools/try_compile_dll.py` 的副本 + `M5GAP_OUT` 环境变量指定输出路径，只读加载，未写游戏目录）。

## 结论摘要

| 缺口 | 性质 | 一行判据 |
| --- | --- | --- |
| ① `{…}` 插值（49 份） | **真的未支持（语义未定）**，不是 C++ 搬运遗漏 | Python oracle 在同一行报同一句话（`expr.py --all 001.si` → `第 11609 行 … U-L3 未决`）；参考编译器同一份源码编出 458276 B（= manifest `out_len`） |
| ② 意外的 ')'（17 份） | **我们的前端比参考严**：参考在 `return` 语句里容忍**多一个 `)`**（产物逐字节相同） | `ret_plain.si`（无多括号）与 `ret_user_stray2.si`（多一个）参考产物 sha256 相同 `f41b2839…`；同一构造写成**语句**（`store(...))`）参考自己 `sysFatal` |

两个缺口都不是独立可解的：17 份里**每一份也含插值**，且插值行在括号行**之后**（例 100：括号 164、插值 1818）——
把 100.si 第 164 行多余的 `)` 删掉后重跑 CLI，报错直接前进到 `第 1818 行 第 23 列：… U-L3 未决`
（`work/tmp/m5gap/100_noparen.si`，命令与输出见下）。所以 **① 是 66 份（49+17）的前置**。

## ① 模板插值

### 1. Python oracle 的行为：同样报错，不能当 oracle

```bash
$ python simplexc/frontend/expr.py --src simplexc/corpus/src/001.si --all
run_sim     label 11575 11727 是 - - - - 失败 第 11609 行：模板字面量的 `{…}` 插值语义未定（U-L3 未决）→ 未支持
get_input   def   11594 11623 是 - - - - 失败 第 11609 行：模板字面量的 `{…}` 插值语义未定（U-L3 未决）→ 未支持
check_output def  11625 11652 是 - - - - 失败 第 11634 行：模板字面量的 `{…}` 插值语义未定（U-L3 未决）→ 未支持
$ python simplexc/frontend/expr.py --src simplexc/corpus/src/100.si --all | tail -2
run_sim  label 1788 1981 是 - - - - 失败 第 1818 行：模板字面量的 `{…}` 插值语义未定（U-L3 未决）→ 未支持
```

原型在 `frontend/expr.py:867-868`（`if t.interp:` → `raise ParseError … U-L3`）主动拒，产品在
`frontend/parser.cc:684-685` 拒同样一句话 —— 是**同一处决定**，不是搬运漏了。100.si 那一跑还顺手证明
② 与 ① 在原型里也是同一个顺序（164 先、1818 后）。

参考侧：`compile_real.dll` 直接编 `corpus/src/001.si` → `rc=0 blob=458276`，与
`corpus/manifest.tsv` 的 `out_len=458276` / `ref_match_rebased=yes` 对上 → **参考支持插值，我们产品不支持**。

### 2. 语义已测出来（不需要实现，用参考产物 + 宿主观测）

驱动方式（三个都可复用）：

1. 片段 = `work/tmp/ul7x/base.si` 的头部（452 行，含 `ui_buffer`/`#SIMULATION_STATE` 地址表、
   corpus 自己的 `def set_text(text: Str, offset: Int)`）+ 探针 `def` + 顶层调用 `exp_xxx()`；
2. `M5GAP_OUT=out.bin python work/tmp/m5gap/tcdll.py --compile --src <片段>.si` → **参考产物**
   （日志 `[2]` 那行是 entry）；
3. `python simplexc/tools/host/host.py --src <片段>.si --bin out.bin --entry <上一步> --ticks 2 \
   --seed-reset-data seq --dump-pages <dir> --out <jsonl> --quiet`；
4. **观测点** = 页快照里 `ui_buffer(=0x232aa930000)` 的 `+offset`：`set_text` 先写 U64 长度、再写字符
   （`store(ui_buffer+offset, length)` / `store(ui_buffer+offset+8+i, U8 text[i])`），所以 `+k` / `+k+8` 直接读出长度与字节。

`t0002_0232aa930000.bin`（`exp_interp` / `exp_interp2`，`work/tmp/m5gap/pages*/`）：

| 模板 | 观测（len, bytes） | 读出 |
| --- | --- | --- |
| `` `A{exp_v}C` ``（`Int 1234`） | 6, `b'A1234C'` | 文字段原样保留 |
| `` `{exp_v}` `` | 4, `b'1234'` | **Int → 有符号十进制** |
| `` `L{exp_v}M{exp_v}N` `` | 11, `b'L1234M1234N'` | 多处插值按序拼接 |
| `` `xy` ``（无插值） | 2, `b'xy'` | 与现在已支持的路径一致 |
| `` `n{Int -7}` `` | 3, `b'n-7'` | 负数带 `-` |
| `` `u{U64 18446744073709551615}` `` | 21, `b'u18446744073709551615'` | U64 → 无符号十进制 |
| `` `b{Bool true}` `` | 5, `b'btrue'` | Bool → `true`/`false`（不是 1/0） |
| `` `k{U1 1}` `` | 2, `b'k1'` | **U1 不是 Bool**，按数字渲染 |
| `` `s{Str "hi"}` `` | 3, `b'shi'` | Str 原样接（不加引号） |

另外三条（同口径）：

- **不调用任何运行时**：这两个片段的宿主快照 `extern` 全为 `[]` → 插值/十进制转换是**产物内联代码**，
  没有堆分配器、没有字符串运行时桩。
- **结果存在栈上**：`let s = \`A{exp_v}C\`` 后 `raw_cast(<U64>, s)` = `0x1da18400028`，落在宿主**线程栈**
  （不在任何被转储的游戏内存区），`s.len()=6`、`s[0]=0x41`、`s[2]=0x32` → 长度显式、按字符下标。
- **`+` 拼接存在且同形**：`let s = "ab" + "cd"` 参考编译成功，`len=4`、`set_text` 收到 `b'abcd'`。

### 3. 修法最小集合（产品侧要定的语义）

1. 模板字面量 → 类型 `Str` 的**表达式**（可在实参、`let/var` 右侧、`+` 链里用；语料三种都有）。
2. 值渲染：Int/I32/S32 → 有符号十进制；U8/U16/U32/U64/U1 → 无符号十进制；Bool → `true`/`false`；Str → 原样。
3. 拼接：文字段与各段值顺序连接；`+`（Str, Str）→ 新 Str。
4. 表示与生命周期：**长度显式**（字符数）、数据放**栈缓冲**、**不需要 NUL 终止**（语料里所有消费方都
   是 `.len()` + `s[i]` + 传给 `set_text`/`ui_set_text`；`set_text` 就是按长度逐字节拷）。所以 U-L3
   里「Str 是否 NUL 终止」这一问对本缺口**不构成阻塞**（参考产物自己也没依赖终结符）。

语料用量（49 份，290 处 / 79 个不同内插式）：内插式上下文分布 = `,` 178 · `(` 104 · `=` 8 · `+` 4 ·
`return` 1；内插值里出现 Str（`get_binary_repr(...)`）、Int 表达式、`U8/U1` 字段、成员访问。

**还差什么才能改**（要判它需要什么实验）：U8/U16/U32/S32 的十进制渲染没直接测（U64/U1/Int/Bool/Str 测了），
但同一驱动 + 一个片段即可（照抄 `interp_probe2.si` 换类型，一次编译一次宿主运行约 30 s）；
以及十进制转换在我们的 lowering 里用什么实现（参考是内联代码，无 extern 可抄）。

## ② 表达式里出现意外的 ')'

### 1. 最小复现（`work/tmp/m5gap/stray_bad.si`，第 2 行 = 100.si:164 原样）

```text
def get_ram_value(label: Str, address: Int, size: @Size) @Size {
    if label == 'Program' { return load(type_of(size), #DATA_1948035650992482295 + (256 - ((size(size) + 7) / 8)) - address)) }
}
run_sim: while true {
    get_ram_value('Program', 0, <U8>)
}
```

```bash
$ build/simplexc_cli.exe --src work/tmp/m5gap/stray_bad.si --out work/tmp/m5gap/stray_bad.bin
语法错误：第 2 行 第 125 列：表达式里出现意外的 ')'      rc=1      ← 与 100.si 的 164:125 同列
$ build/simplexc_cli.exe --src work/tmp/m5gap/stray_ok.si --out work/tmp/m5gap/stray_ok.bin   # 同一个字面去掉多余 ')'
UNSUPPORTED: 编译期类型参数 Size @ 第 1 行                 rc=1      ← 语法已过，卡在后面（泛型单态化）
```

构造本体（100.si:164，`(`=6 / `)`=7，唯一不平衡处就是第 125 列那个 `)`）：
`if label == 'Program' { return load(type_of(size), A + (256 - ((size(size) + 7) / 8)) - address)) }`。
17 份全都同形：唯一不平衡行都含 `return`、每行净差 −1、括号位置都在 `return` 表达式尾部。

### 2. 判错位置（产品）

`frontend/parser.cc:719` 抛（`parse_primary` 的兜底），路径是：`parse_return`（`parser.cc:463-471`）
解析完 `return` 表达式后，块循环把遗留的 `)` 当**下一条语句**的起点 → `parse_expr` → `parse_primary` → 抛。
原型同构：`frontend/expr.py:889`（同样文案）。

### 3. 是「真缺陷」还是「不支持的构造」——参考实测（2×2 单因子对照）

`work/tmp/m5gap/` 里同一表达式（`load(<U64>, Ptr 8192 + (128 - ((a + 7) / 8)) - a)`）只改两处：
「语句 = `store(...)`」vs「`return`」、多一个 `)` vs 不少；每份加 `f(1)` 迫使参考分析函数体：

| 片段 | 形状 | 参考编译器 |
| --- | --- | --- |
| `ctl_sameline.si` / `ctl_nextline.si` | 无多括号 | 产物 8597 B |
| `stray_sameline.si` / `stray_nextline.si` | `store(...))`，多一个 `)` | `sysFatal`：`front_end.nim(2688, 28) 'false' Missing open paren (TODO make this an error) [AssertionDefect]` |
| `stmt_user_stray.si` | 用户函数当语句，多一个 `)` | 同上 fatal（不是 `store` 内建的特殊行为） |
| `stray2_sameline.si` | 多两个 `)` | 同上 fatal |
| `ret_ctl.si`（多一个 `)`）/ `ret_user_stray2.si`（多一个 `)`）| `return … )` | 编译成功 |
| `ret_plain.si` | 同上去掉那个 `)` | 成功，且与上一行**逐字节相同**：`f41b2839b25cf6a9…` |

→ 参考的规则是：**`return` 表达式后再紧跟一个 `)` 时，它被静默吃掉（对产物零影响）；其它位置（表达式语句）多一个 `)` 则进程 fatal**。
这份语料的 17 份正好落在被容忍的形状里（100.si 整份用参考编出 70526 B，与 manifest `out_len` 一致）。

所以 ② 既不是「语料坏了」，也不该按「普遍容忍多余括号」来修：**窄修**才对得上参考。

### 4. 修法选项与代价（②）

| 选项 | 改哪些文件 | 规模 | 风险 |
| --- | --- | --- | --- |
| A（推荐）`parse_return` 里吃掉紧随的那个 `)` | `frontend/parser.cc`（`parse_return`，~4 行 + 一条注释说明参考为何容忍、语料哪 17 份） | 极小，一处；加一个以源文件命名的用例 | 只对「`return` 表达式后紧跟 `)`」放宽 → 与参考一致；`}` 之后/语句位置照旧报错 |
| B 全局把 `)` 当空表达式/忽略 | `parse_primary` | 小 | **过度接受**：参考在语句位置是 fatal 的，我们的壳/失败路径语义会跟着变，且掩盖真错误 |
| C 不动前端，预处理剥掉行尾多余 `)` | 需要新的预处理步骤 | 中 | 参考是按「return 表达式」判的，按行剥会改掉别的构造；且绕过冻结的语法层 |

**还差什么**（要判它需要什么实验）：`return <expr>)` 后面**还有别的 token**（例如 `return load(…)) + 1`）
参考怎么走没测；语料里不存在这种形状，窄修不必覆盖，但要在实现时写死口径。

## 建议顺序

1. **先 ①**（49 份 + 17 份的前置；语义的关键数值已测出，见上表）。
2. 再 ② 的选项 A（4 行级，独立、可单独验收；做完 17 份仍需 ① 才能装出来）。
3. 顺手补的两个小实验：U8/U16/U32/S32 渲染（一个片段）；`return` 后多余 `)` 再跟 token 的行为。

## 与并发写者的关系

另一个 worker 在改 `simplexc/lower/**` 并重建 `build/simplexc_cli.exe`；本次跑 CLI 期间**未**遇到
「exe 被截断 / 与语法无关的错」，没有重跑。

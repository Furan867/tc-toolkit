# cli_fail 28 份的逐类诊断（只读产品代码）

诊断者（read-only 产品代码；未改任何 `simplexc/**`）。器材与原始片段：`work/tmp/clifail/`。
参考侧一律用 `compile_real.dll`（`work/tmp/m5interp/referr.py`，只读加载，未写游戏目录）。

## 口径与一个必须知道的坑

参考 `compile()` 对「没有 `run_sim:` 根块」的小片段**返回固定 8581 B 的桩**（`entry=0x212f`），
大小不随源码变 —— 所以最小片段只能给出**接受 / 拒绝**这一个信号，不能比产物。
该信号本身是可信的：对照 `ctl_undef.si`（调用不存在的 `zzz_undefined`）参考报 `Compile error`（见下）。
故下面所有「参考接受」都指：**参考不报错**（说明该构造在参考里存在），而不是产物等价。

产品侧判据命令（片段的原始输出见 §逐类）：

```bash
$ build/simplexc_cli.exe --src work/tmp/clifail/<片段>.si --out work/tmp/clifail/out.bin
$ PYTHONIOENCODING=utf-8 python work/tmp/m5interp/referr.py work/tmp/clifail/*.si | tee work/tmp/clifail/referr.log
```

参考侧原始输出（`work/tmp/clifail/referr.log`，流水）：

```text
brace_bare.si 成功 8581 · brace_ctrl.si 成功 8581 · ui_hidden.si 成功 8581 · ui_hidden_true.si 成功 8581
ui_hidden_expr.si 成功 8581 · array_builtin.si 成功 8581 · ror_builtin.si 成功 8581 · random_builtin.si 成功 8581
prefix_only.si 成功 8610（randomize_seed，只在前缀里）· emptyarr.si 成功 8581 · constnop.si / constnop_local.si 成功 8581
switchstr.si 成功 8581 · streq.si 成功 8581 · ctl_undef.si 失败 err='Compile error'
```

## 总表

| 类 | 份数 | 性质 | 产品判错位置 | 规模 | 修完后下一道（实测） |
| --- | --- | --- | --- | --- | --- |
| 裸块 `{ … }` | 12 | **缺功能**（不是插值引入） | `frontend/parser.cc:305-341` 无 `{` 分支 → `:723` 抛 | 6 文件 ~40 行 | `array`（147 实测） |
| `ui_set_hidden` 重载 | 6 | **缺陷**（`static_type` 少 true/false） | `lower/lower.cc:1496-1512` → `:1670` | +2 行 | Str-switch(075/076/077/078) · random(108) · 嵌套 def(116) |
| `array` | 3（+12 的第二道） | **缺功能**（参考内建；前缀与 98 份都无 `def array`） | `lower/lower.cc:1672` | 中（~20 行 + 1 个决定） | random(084/096/098) |
| switch 主体 Str | 2（+7 类的第二道） | **缺陷**（ptr 比 i64）+ 真修要**缺**的 Str 相等 | `lower/lower.cc:1034-1053` | 中（~40 行 + 1 个决定） | — |
| `ror` | 2 | **缺功能**（参考内建） | `lower/lower.cc:1672` | ~20 行（仿 `builtin_asr:1745`） | 未见其它缺口 |
| `random` | 1（+11 的第二道） | **前缀函数** → 路线 A，不是独立缺陷 | `lower/lower.cc:1672` | 大（=喂前缀） | — |
| 空数组字面量 | 1 | **缺功能** | `lower/lower.cc:2479` | ~15 行 + 1 个决定 | array（157 还有 `array` @3968） |
| 局部 `const #N` 折叠 | 1 | **缺陷**（`cv` 不查作用域） | `lower/lower.cc:2092-2110` → `:874` | +3 行 | random（102 下一道 @3114） |

**结论先行**：这 28 份不是 8 个独立缺口，而是**一条依赖链**。实测每条链（`work/tmp/clifail/chain*.py`，
在工作副本上手工绕过一类、看下一个错）：

```text
147（12 份代表）：裸块 ×4 → array@806 → …
075（6 份代表）：  true/false → Str-switch(IR 校验) → …
108：              true/false → random@3683
116：              true/false → 嵌套 def 调用嵌套 def(is_wall)@3832   ← 第 9 类，覆盖表里没记
024：              Str-switch 一道                ← 唯一一道
178/179：          ror 一道
114：              random 一道
```

## 逐类

### 1. 裸块 `{ … }`（12 份：147/149/159/162/163/164/165/167/168/171/172/174）—— 缺功能

最小复现 `work/tmp/clifail/brace_bare.si`（**不含任何反引号模板**）：

```si
def f(x: Int) Int {
    var y = x
    {
        var z = 2
        y = y + z
    }
    return y
}
```

- 产品：`语法错误：第 3 行 第 5 列：表达式里出现意外的 '{'`
- 参考：`成功`。对照组 `brace_ctrl.si`（同一函数去掉裸块）产品 `blob 760 B` —— 差异只在裸块。
- 判错位置：`parser.cc:305 parse_statement` 对 `{` 没有任何分支，落到 `:340 parse_assign_or_expr`
  → `parse_expr` → `primary` 在 `:723` 抛。`parse_block()`（`:285`）只被 if/while/switch/def 调。
- **是不是插值引入的？不是。** 三条证据：① 本片段没有模板，报同一句话；② Python 原型
  `frontend/expr.py:588 parse_statement` 也没有裸块分支 —— 前后端**同一处决定**，不是 C++ 搬运漏；
  ③ 12 份的报错行（1064/2118/2222/2130…）**全都是** `        {`（列 9，独立一行），每份都有 3–4 个，
  而模板都在更前面的行（147 的模板在 1048）。插值只是让解析**走过了**模板行，把本来就有的缺口露出来。
  （12 份在更早的基线里报的是第 158 行多一个 `)`，那一道由 `d5c9e82` 修掉，与插值无关。）
- 规模：`ast.hh` 加 `NK::BlockStmt` + `frontend/parser.cc:305` 加分支（~6 行）+
  `lower/lower.cc:848 lower_stmt` 加分支（`lower_block` 已自带 scope push/pop，`:842`）+ 4 处遍历
  switch（`lower.cc:~150 for_each_child`、`frontend.cc:~215 walk_nodes`、`ast_dump.cc:228/:499`）+
  `lower.cc:411 kind_name`。≈6 文件 40 行。
  ⚠ `for_each_child` 与 `walk_nodes` 的 `default: break` 是**静默**的 —— 只加 parser/lower 不加这两处，
  编译能过、但可达性/覆盖统计会漏掉裸块里的语句（`lower_stmt` 的 default 是 throw，安全）。

### 2. `ui_set_hidden()` 重载解析（6 份：075/076/077/078/108/116）—— 缺陷

最小复现 `work/tmp/clifail/ui_hidden.si`：

```si
def ui_set_hidden(id: Str, value: Bool) Bool { return value }
def probe() Int { let h = ui_set_hidden("table", false) ... }
```

- 产品：`UNSUPPORTED: ui_set_hidden() 的重载解析拿不到参数类型 @ 第 6 行`
- 参考：`成功`。**判据对照**（同一文件只换实参写法）：

  | 片段 | 第 2 个实参 | 产品 |
  | --- | --- | --- |
  | `ui_hidden.si` | `false` | 拿不到参数类型 |
  | `ui_hidden_true.si` | `true` | 拿不到参数类型 |
  | `ui_hidden_expr.si` | `1 == 0`（Bool 表达式） | `blob 760 B` ✅ |

- 判错位置：`static_type()`（`lower.cc:1496`）的 `NK::Ident` 分支（`:1506-1528`）查了
  lookup/globals/prefix/enum/consts/hash_consts，**唯独没有 `true`/`false`** → 返回 nullptr →
  `compatible(nullptr, Bool)` 假 → `pick_def` 无候选 → `call_named:1670` 抛。
  `e_IdentExpr:1153` 与 `cv:2094` 都特判了 true/false，只有 `static_type` 漏 —— **三处口径不一致，是缺陷**。
- 规模：`lower.cc:1506` 前加 2 行（`if (n.name == "true" || n.name == "false") return ty_bool();`）。
- 修完解 0 份：075/076/077/078 下一道是 Str-switch（IR 校验失败），108 是 `random`，116 是嵌套 def。

### 3. `array(<T>, n)`（3 份：084/096/098；且是 12 份的第二道）—— 缺功能

最小复现 `work/tmp/clifail/array_builtin.si`：`var a = array(<Int>, 4)` → 产品
`UNSUPPORTED: 未定义的函数 array @ 第 2 行`；参考 `成功`。
语料原文：`084:1050 var instructions = array(<Int>, 256)`（在 `run_sim` 根块里）；
`096:1641`/`098:1688` 是 `array(<U8>, 256)`。

- 判错位置：`call_named`（`lower.cc:1672`）—— 内建表（`:1660-1666` 的 load/store/raw_cast/asr/minmax）里没有 `array`。
- **缺功能**：`grep` 前缀（`corpus/stdlib_prefix.si`，1503 行）与 98 份语料**都没有 `def array`**，
  参考却接受 → 它是 `compile_real.dll` 的内建/宏（前缀里同样只有零命中的还有 `grow`/`shrink`/`join`/`empty_array_address`）。
- 规模：~20 行（`alloca` n×sizeof(T) + 清零 + 返回 `Arr[T]` 指针）**+ 一个决定**：参考的数组是不是堆分配、
  `len()` 从哪里来。对本 3 份而言栈上够：084 的 `instructions`（`:1050-1083`）只有定值写与
  `instructions[cycle]`/常量下标读，**没有 push/grow/len**。保真度未验，别默认栈实现。
  另：`array()` 有**运行期长度**（`#MAX_RAM` 是 const，`256` 是字面量，本 3 份都能在编译期定长）。

### 4. switch 主体是 Str（2 份：024、151；另是 §1/§2 的下一道）—— 缺陷 + 缺的语义

最小复现 `work/tmp/clifail/switchstr.si`：

```si
def probe(s: Str) Int {
    switch s
        case "a" { return 1 }
        default { return 0 }
}
```

- 产品：`编译失败：IR 校验失败：Both operands to ICmp instruction are not of the same type!` +
  `  %sc = icmp eq ptr %ld, i64 0`；参考 `成功`。
- 判错位置：`s_SwitchStmt`（`lower.cc:1034`）：`:1036` 取 `subj_ll`（Str → `ptr`），`:1053`
  拿 `const_int(pat_const(case 值).value_or(0))`（Str 常量 → `i64 0`）直接 `LLVMBuildICmp`。
- 性质：**表面是缺陷（类型不匹配），真修是缺功能** —— 参考对 Str 主体显然按**内容**比较（`075:898` 那种
  「按名字派发 UI 元素」的用法要求如此），而我们**没有 Str 相等**：`streq.si`（`a == b`，两个 `Str`）
  **产品静默编过**（ptr 与 ptr 的 ICmp）→ 现在是一条**静默错码**路径，不是报错。（可疑：与 40 份 TickDiff 有关，值得单独查。）
- 规模：Str 相等（长度 + 逐字节比较，内联 ~25 行）+ `s_SwitchStmt` 的 Str 分支（~15 行，
  case 值按内容比；或先按长度筛）。**+ 一个决定**：Str 相等语义（U-L3 里「Str 是否 NUL 终止」相关项）。
- 单独修这一类解 1 份（024）；151 还要 `random`。

### 5. `ror`（2 份：178、179）—— 缺功能

最小复现 `work/tmp/clifail/ror_builtin.si`：`return Int ror(U32 8, 1)` → 产品
`UNSUPPORTED: 未定义的函数 ror @ 第 2 行`；参考 `成功`。
语料原文：`178:5760 ... = ror(load(<U32>, ...), (U32 (load(<U5>, ...))))`、`179:4862`（4 处/份）。

- 判错位置：`lower.cc:1672`（同 §3）。前缀与语料都无 `def ror`（前缀 671 行那处是注释掉的 `big_int_ror`）→ 参考内建。
- 规模：~20 行，照抄 `builtin_asr`（`lower.cc:1745`）的形状。**先探语义**：旋转量是否按 W 取模、
  `ror(x, 0)`/`n ≥ W` 的行为（`asr` 是「按 W 饱和」，`>>` 是「按 W 取模」，两者不同，别猜）。
- 178/179 的扫描里**没有**其它已知缺口（无 array/random/空数组/裸块/字符串 case）→ 修完这两份可能直接可装配。

### 6. `random`（1 份：114；另是 11 份的第二道）—— 前缀函数，归路线 A

最小复现 `work/tmp/clifail/random_builtin.si`（`return random(256)`）→ 产品
`UNSUPPORTED: 未定义的函数 random @ 第 2 行`；参考 `成功`。
定义在 `corpus/stdlib_prefix.si:1491 def random(max: Int) Int`（`:1486 def random() Int`）。
判据：`prefix_only.si`（只调 `randomize_seed()`，该名只存在于前缀 `:1477`）参考也 `成功` →
**参考的编译环境里有整个前缀**（`compile.dll` 自己喂的），不是 `random` 特例。
- 判错位置：`lower.cc:1672`（我们从不喂前缀）。
- 规模：这不是单点修复，**是路线 A（前缀 + 用户源码当一个整体）**；单点实现 `random` 只是把 114 推进
  到「下一个前缀名字」，不建议。

### 7. 空数组字面量（1 份：157）—— 缺功能

最小复现 `work/tmp/clifail/emptyarr.si`：`var outputs = Arr[U8] []` → 产品
`UNSUPPORTED: 空数组字面量 @ 第 4 行`；参考 `成功`。
- 判错位置：`array_literal`（`lower.cc:2476`），`:2479 if (ev.empty()) throw`。
- AST：`Arr[U8] []` = `CastExpr(target=Arr[U8], expr=ArrayLitExpr(items=[]))`（`ast_dump` 原始输出见上），
  所以**元素类型在 cast 上**，落点应是 `e_CastExpr`/`convert` 侧特判，而不是在 `array_literal` 里猜。
- 规模：~15 行 + 一个决定（零长数组怎么表示：长度头要不要、与 `make_array` 的指针表示是否一致）。
- 157 还有 `array`@3968 → 这类修完 157 仍卡在 `array`。

### 8. 局部 `const #N` 的初始化式（1 份：102）—— 缺陷

最小复现 `work/tmp/clifail/constnop_local.si`（**对照组 `constnop.si` 把三个 const 放顶层就编过**）：

```si
def probe() Int {
    const #COND  = U8 0b1100_0000
    const #NEVER = U8 0
    const #NOP   = #COND | #NEVER      ← 产品：编译期常量 #NOP 的初始化式不是常量
```

- 产品：`UNSUPPORTED: 编译期常量 #NOP 的初始化式不是常量 @ 第 5 行`；参考 `成功`。语料 `102:3108` 同形（在 `run_sim` 根块里）。
- 判错位置：`cv()` 的 `NK::Ident`（`lower.cc:2092-2110`）只查 `consts_`（= 顶层常量表）/
  `enum_member_`/`hash_consts`，**不查作用域** `lookup()`；抛在 `s_VarDeclStmt:874`。
  同文件里 `e_IdentExpr:1160` 与 `static_type:1508` 都查了 `lookup` 的 `Const` 绑定 → **又是口径不一致的缺陷**。
- 规模：+3 行（`cv` 的 Ident 分支先 `lookup`，`Kind::Const` 直接返回 `*b->value`）。
- 102 的下一道是 `random`@3114 → 这类单独修也只解第一道。

## 第 9 类（覆盖表里没有，§2 修完后才露出）

`116`：`def is_wall` 是**嵌套 def**（`116:3804`，缩进 4），被另一个嵌套 def 调用（`:3832`）→ 产品
`UNSUPPORTED: 嵌套 def 调用另一个嵌套 def（is_wall）@ 第 3832 行`，抛在 `lower.cc:1105`
（`captures_of`）。性质：缺功能（嵌套 def 的闭包/提升）。规模未估（先确认只有 116 一份）。

## 建议修复顺序（按「能解几份 / 成本」）

1. **`true`/`false` 进 `static_type`**（2 行，6 份的第一道，零语义风险）。
2. **局部 `const` 的 `cv` 查作用域**（3 行，1 份，与 ① 同一形状的缺陷，顺手）。
3. **`ror`**（~20 行，2 份，可能是 178/179 的**唯一**缺口 → 直接 +2 可装配；先探旋转语义）。
4. **裸块**（6 文件 ~40 行，12 份的第一道）—— 与 5 一起才是这 12 份的完整门票。
5. **`array`**（~20 行 + 1 个决定）—— 12+3 份的公共瓶颈；先探参考的数组内存/长度语义再派。
6. **Str 相等 + switch 的 Str 分支**（~40 行 + 1 个决定）—— 单独解 024；且顺手堵掉
   `Str == Str` 静默比指针这条错码路径（可能与 40 份 TickDiff 相关）。
7. 空数组字面量（~15 行，1 份）。
8. `random` / 前缀 / 嵌套 def：不是单点，归路线 A 与后续里程碑。

## 遗留风险

- 参考小片段只给「接受/拒绝」信号（固定 8581 B 桩）；产物等价、`array` 的堆/长度语义都**没验**。
- 「哪些 def 会进 lowering」的规则没完全吃透（024 的 `ui_set_position` 明明没人调用却编到了 IR；
  116 的嵌套 def 才炸）→ chain 的先后顺序可能随 lowering 顺序变，只是本次实测如此。
- `Str == Str` 静默错码（`streq.si` 编过）—— 建议单独立项查，别混进这次修复。
- 108/116 的下一道分别依赖 `random` 与前缀，所以「26 份可装配」的乐观估计不成立：逐类修会一层层露新缺口。

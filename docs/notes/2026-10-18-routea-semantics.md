# 路线 A 的核心语义（实测：参考编译器 + objdump）

日期：2026-10-18 · 只读产品代码（未改 `simplexc/**`）· 器材与片段全在 `work/tmp/routeasem/**`。
参考侧一律 `compile_real.dll`（`work/tmp/m5gap/tcdll.py`），**不跑宿主**。

器材：`refc.py`（编译 + 反汇编，产物 `<名>.ref.bin` / `<名>.bin.txt`）、`dis.py`（只看「追加区」）。
判据形态：产物**字节增量 + 具体立即数 + 反汇编**；每条都带片段名，可原样复跑。

---

## 0 先说两条器材上的硬事实（影响后面所有实验）

1. **`compile()` 自己前置 1503 行 stdlib 前缀**。证据：3 行片段里写 `type Seed Int` → 报
   `Seed is already declared in this block.`，且报错行号 = 我的行号 **+1503**（1504/1506/1507…）。
   → 片段里**不能重声明**前缀已有的名字；实验里直接用前缀的 `Seed` / `global_seed`。
2. **参考编译器内联 + DCE 极凶，产物里只看得到「可观测」的东西**。空源码基线 = **8581 B**，
   本次源码的代码落在 **0x2185 之后**（`dis.py` 看这段）。
   - 反证：`var o1 = Int 0` + `o1 = f(3)`（没人读）→ **与空源码逐字节相同**（只差 ASLR 指针）。
   - 要留下证据必须让值逃逸：`store(Ptr <绝对地址>, U64 …)` 或喂给 `load` 的地址。
   - 因而「没长字节」不是「没编译」，是「被干掉了」——所有下面的片段都按这条写。

---

## 1 `@Type` / `@Size` / `@Any` 的实例化规则

| 片段 | 写法 | 参考产物证据 | 结论 |
|---|---|---|---|
| `g2_size.si` | `def gs(num: @Size) Int { store(Ptr 0x2000, U64 (size(num)*1000+111)) … }`；调 3 次：`gs(load(<U8>,p8))`、`gs(load(<Int>,p64))`、`gs(load(<U8>,p8))` | 追加区里 3 份内联副本：`mov ecx,0x1faf`(8111=8·1000+111) **两次**、`mov ecx,0xfa6f`(64111=64·1000+111) 一次 | `size(x)` **按实参具体类型折成编译期常数**（8 bit / 64 bit，单位是**位**）；每个**调用点**一份展开 |
| `g2_type.si` | `def gt(t: @Type) Int`；调 `gt(<U8>)`、`gt(<U64>)`、`gt(<U8>)` | 同一形状：8111×2、64111×1 | `@Type` 实参写法是 `<U8>`；`size(t)` 同样折 |
| `g2_type_infer.si` | `gt2(load(<U8>, p8))`（**传值**给 `@Type` 参数） | 8111 出现 1 次 | `@Type` 参数**也能由实参值推出类型**（推成 `U8`） |
| `g2_any.si` | `def gany(x: @Any) Int { … size(x) … }`；调 `gany(load(<U8>,p8))`、`gany(load(<Int>,p64))` | 8111、64111 各 1 | `@Any` 与 `@Size` 同一套：**按实参具体类型实例化**，`size` 折 |
| `g2_nested.si` | `def ginner(t: @Type) Int { store(…, size(t)*1000+111) }`；`def gouter(t: @Type) Int { store(0x2008, ginner(<@Type>)+222) }`；`gouter(<U8>)` | 追加区：`mov rcx,0xfffffffffffffc87`（**-889 = -1000+111**）→ `size(t) == -1` | **`<@Type>` 不把具体类型透传下去**：嵌套实例里类型参数仍是「未定」，`size()` 返 **-1**（哨兵）。`<t>`（参数名）这种转发写法**未测** |

- **同一 (函数, 实参) 是否只实例化一次 —— 未验。** 参考把这类小函数**全内联**（`d5_two.si` 里同一
  `$` 函数两个调用点也各内联一份），共享体在产物里根本不存在，观测不到缓存规则。
- 结论一句话：**要支持它，我们得动前端** —— 加「单态化实例化 + `size()` 编译期折叠」，
  并且照抄「未定类型 → `size = -1`」这个哨兵；嵌套转发目前参考自己也是塌的（别自造语义）。

## 2 `$` 可变参数（接口 U-L6）

| 片段 | 观察 | 结论 |
|---|---|---|
| `d_noref.si` | `g.inner7()`（调用点不写 `$`）→ 报错原文 **`1st will get modified, so put '$' in front when calling it.`** | 调用点的 `$` 是**强制**的，且是**编译期已知「第一实参会被改」** |
| `d3_assign.si` | `dot set5($s: Seed) { store(0x2000,U64 s); s = Seed 99; store(0x2008,U64 s) }`，`$global_seed.set5()`；追加区：`mov ecx,0x1`（= 调用者当前值 1）→ `mov [0x2000]`；然后 `mov ecx,0x63`（99）→ `mov [0x2008]` | 体内读 `s` 拿到的就是**调用者的存储**（别名），赋值后立刻读到新值 |
| **写回时机（每次赋值 or 返回）—— 未验。** | 所有 `$` 链都被内联 + SROA：`d5_two.si` 里 `global_seed` 整个变成了寄存器 `rax`，内存里没有写回点可看 | 观测不到；但「调用点强制 `$`」+ 别名读语义 ⇒ **实现上按「引用/别名」做是对的** |
| `d2_plain.si` | `inner7(s)`（把 `$` 参数当普通实参）→ `No function matched inner7(Seed).` | `$` 参数**不是** `Seed` 类型的普通左值 |
| `d6_chain2.si` | `dot outer8($s: Seed) { $s.inner8() }` → **只有一行 `COMPILER ERROR`，没有行号** | 标量的 `$`→`$` 转发被拒（且是「裸报错」） |
| `d6b_chain3.si` | 同一句改写 `inner8($s)` → `No function matched inner8(Seed).` | 两种写法都不行 |
| **反例（前缀自身）** | `stdlib_prefix.si:1084` `def sort($arr: Arr[@Any]) { quick_sort($arr) }` —— 前缀**每次都编过**（任何片段都带着它） | **数组类型**的 `$` 参数可以「原样转发」（`quick_sort($arr)` 带 `$`）；与标量的结论冲突 → **转发规则未验到底** |

- 结论一句话：**要支持它，我们得动 lowering** —— `$` 形参在 ABI 上是**引用**（别名），调用点必须标 `$`
  （前端做「强制 `$` + 报错文案」），而「`$` 参数再传给 `$` 参数」这条**先按数组能过的形状试**，
  标量形状要再测一轮。

## 3 `asm` 块的行为

| 片段 | 参考产物（追加区反汇编） | 结论 |
|---|---|---|
| `a2_asm.si`（含 `rdtsc` / `mov rcx,32` / `shl rdx,cl` / `mov $result,rax`） | `rdtsc` 原样；`mov rcx,32` → `mov rdx,rcx` + `mov ecx,0x20`（**块周围插了保存/恢复**）；`shl rdx,cl` 原样；`mov $result,rax` → `mov rbx,rax` + `mov rbp,rbx`（**`$name` 解析到该变量的虚拟寄存器**） | **不是原样粘贴字节**：块被**重新汇编**，寄存器名按编译器自己的分配重写，名字操作数（参数/局部/`$局部`）被替换成寄存器 |
| `a2_memcpy.si`（调前缀 `memory_copy` / `memory_clear`） | `mov esi,0x1000` / `mov edi,0x1008` / `mov ecx,0x4` / `rep movs BYTE PTR es:[rdi],BYTE PTR ds:[rsi]`；`mov eax,0x0` + `rep stos` | 参数名 `source/destination/length` 被换成**实参寄存器/立即数**；`rep movsb` 原样；`mov al,0` 被规范化成 `mov eax,0x0`。「寄存器约束」就是**按名字替换**，没有独立约束语法 |
| 前缀里可参考的最小样例 | `memory_copy`(186)、`memory_clear`(200)、`memory_copy_reverse`(214) 三段 x86_64 块；`get_cycle_count`(565) 用 `$result`；143 `breakpoint` | 前缀 45/143 行的 `asm aarch64` 整块在源里**被注释掉**（活的只有 x86_64） |

- 结论一句话：**要支持它，我们得动前端 + 一个小编译器** —— 需要一个认识「名字操作数」的汇编器
  （把变量名映射到 lowering 分配的寄存器/栈槽 + 块前后保存恢复），不能只当字符串塞进 IR；
  范围可以只覆盖前缀用到的指令。

## 4 顶层算子单元（`binary +` / `dot …` / `index_read`）

| 片段 | 观察 | 结论 |
|---|---|---|
| `op2_bin.si` | `binary + (a: Int, b: Int) Int { store(0x2000,4242); return 0 }`，调用点 `load(<Int>,p) + 1` → 追加区**没有 add/lea**，只有一次 4242 存储 + 存 0 | `binary +` **接管**了 `Int` 上的 `+`（优先级高于内建），并按**实参类型**解析 |
| `op3_dot.si` | `dot foo(x: Int) Int { store(0x2000,4343); return 0 }`，调用点 `load(<Int>,p).foo()` → 一次 4343 + 存 0 | `dot` 单元按**接收者类型**解析，与 `binary` 同一套重载 |
| `op2_dot.si`（写错） | `U64 load(<Int>,p).foo()` → `No dot call matched U64.foo(). Perhaps you meant: dot foo(x: Int) Int { … Got U64` | 解析用的是**表达式静态类型**，报错会给出候选签名 |
| `op2_index.si` | `index_read (input: Arr[Int], index: Int) Int { store(0x2000,4444) … }`，调用点 `a[0]` → 生成的是**内建索引读**（`imul rax,rax,8` + `mov rdx,[rcx+rax]`），**没有 4444、没有 call** | `index_read` **没有**覆盖 `Arr[Int]` 上的内建索引（**未验**：Str 那两个前缀声明是否真在用） |

- 结论一句话：**要支持它，我们得动前端** —— 建一张「算子单元表」，把 `a+b` / `a.f()` / `a[i]` / `a[i]=v`
  在调用点改写成对单元的调用（= 重载解析的一种，成本低）；`index_read/index_write` 的生效条件要另测。

---

## 遗留（都写了复现方式，没敢下结论）

1. **同一 (函数,实参) 是否只实例化一次** —— 未验（参考全内联，看不到共享体）。
2. **`$` 写回时机**（每次赋值 vs 返回）—— 未验（内联 + SROA 后调用者变量在寄存器里）。
3. **`$` 参数转发给 `$` 参数**：标量被拒，数组（前缀 `sort`→`quick_sort($arr)`）能过 —— 差异未验。
4. **`index_read`/`index_write` 何时真的接管** —— 未验（`Arr[Int]` 不接管）。
5. `@Type` 转发的另一种写法 `<t>`（参数名）**未测**；`<@Type>` 已测（塌成 `size = -1`）。
6. 副作用观察：`var seedval = Seed 1` 后在函数体里读它 → `Variable seedval does not exist.`；
   同形状的 `var qq = Int 1` 却可以 —— **记下未追**。
7. **参考实现自己崩/烂报错的形状**（见到就绕）：
   - 语法错（`def f( {`）→ `front_end.nim(2728) AssertionDefect`，进程 rc=1、**无产物**；
   - `$s.inner8()`（把 `$` 参数当接收者再转发）→ **只有一行 `COMPILER ERROR`，没有行号/列号**。
   - 判据只用「有没有产物（blob 字节数）+ 报错原文」，不要解析行号以外的结构。

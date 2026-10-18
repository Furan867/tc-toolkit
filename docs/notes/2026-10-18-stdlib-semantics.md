# stdlib 前缀的语义地图（A）与 `random` 实测（B）

日期：2026-10-18 · 只读产品代码（未改任何 `simplexc/**`）· 工作树 `90b9b6c`（当时 `docs/notes/2026-10-18-m5-coverage.md` 有未提交改动；
`simplexc/frontend/**`、`simplexc/lower/**` 另有写者，**下面的支持性结论只对这一次快照有效**）。
对象：`simplexc/corpus/stdlib_prefix.si`（1503 行 / 27656 B，与 `work/hook/all/stdlib_prefix.si` 逐字节相同 —— 游戏喂给编译器的就是它）。

器材与片段全在 `work/tmp/stdlib/**`；**参考侧一律 `compile_real.dll`（`work/tmp/m5gap/tcdll.py`），宿主 `simplexc/tools/host/host.py`**。

---

## 0. 结论先行

1. **前缀 = 14 块**，不是「语法杂」：真正的门槛是 5 类**语言构造**缺口 + 7 个**内建**缺口（§A2 / §A3）。
2. 5 类构造缺口（按「挡住前缀的位置」排序）：`$`（**第 172 行就挡死，`binary +=` 处**）· 泛型编译期类型参数 `@Any/@Type/@Size` · `[T] expr` 方括号转换 · `extern` 命名空间 / `.'成员名'` 引号成员 · 多行 `Enum[`。
3. **`random` 语义已定死**（§B）：`state = xorshift64*(state) × 0x02545F4914F6CDD1`（**前缀写的是 15 位十六进制常数**，不是教科书的 `0x2545F4914F6CDD1D`），初始 `state = 1`，`random()` 返回新 state 全 64 位，`random(n) = U64 state % U64 n`（`[0,n)`；`n=1` 恒 0；`n=0` 返回被除数；`n<0` 先按 U64 重解释）。
4. **只做路线 B，`random` 能单点实现**：不需要 arena、不需要 `array`/`size`/`@Type`，只需要「一个 8 字节程序全局 + 一条 4 行算术」，且**必须照抄那个 15 位常数**（§B5）。
5. `get_time`/`sleep` 与我们的 lowering **语义一致**（常数逐个对上，§B6）。

---

## A1. 模块清单（1503 行怎么分块）

行号取 `grep -n '^(def|type|const|binary|dot|extern|call_conv|index_|var) '`。

| # | 行范围 | 块 | 入口（行号） | 备注 |
|---|---|---|---|---|
| 1 | 1–43 | 常量与类型头 | `#INT_HIGH`(2)…`#S8_LOW`(15)；`type Char`(17)；`Time/Seed/File/Ptr`(38–41)；`Str/Cstring`(42–43) | `Char` 是 **256 员多行 Enum**（内含 `// 32`…`// 128` 注释行） |
| 2 | 45–166 | 调试与断言 | `print_using_stack(@Any)`(45)、`breakpoint()`(143)、`assert`(158) | `print_using_stack` 的活体是 **`asm aarch64`**，x86_64 那份整块被注释掉 |
| 3 | 168–183 | 指针算术单元 | `binary + / += / - / -=`(`Ptr`,`AnyInt`)(168/172/176/180) | 3 个 `$` 参数在这里 |
| 4 | 186–240 | 内存原语 | `memory_copy`(186)、`memory_clear`(200)、`memory_copy_reverse`(214) | x86_64 asm（`rep movsb` / `rep stosb`） |
| 5 | 243–295 | 系统层声明与 win32 包装 | `call_conv`(243)、`extern windows_x64 kernel32`(251)、`exit`(253)、`memory_reserve/commit/free`(257/267/278)、`thread_exit`(289) | 全部走 `kernel32.'引号成员名'` 调用 |
| 6 | 297–528 | arena 与数组运行时 | `to_capacity`(297)、`get_capacity`(301)、`full_aray_size`(305)、`set_length`(309)、`get_length`(314)、`allocate_raw`(318)、`__allocate_data/clear`(363/375)、`__create_arenas/__free_arenas`(385/401)、`__shrink/__grow/__clone/__append/__join`(416/427/441/451/464)、`__thread_exit`(477)、`dot push/pop/cstring`(483/491/507)、`assert`(515，与 158 行**重复定义**) | arena 头 24 B 三段（`ptr@0 / cap@8 / top@16`），数组头 `p-8` 低 16 位是 cap_log2、高 16 位是长度（源里写作 `(len << 16) | cap_log2`），`p-2` = U16 容量 |
| 7 | 530–576 | I/O 与时钟 | `#STD_*_HANDLE`(530–532)、`output`(536)、`sleep`(546)、`get_time`(553)、`dot write(File,Arr[U8])`(562)、`get_cycle_count`(565) | `output` 用 `kernel32.'_write'(text,length)`（**2 个实参**） |
| 8 | 580–616 | 格式化辅助 | `hex(num: @Size)`(580) | 靠 `size(@Size)` + `array` + `[U8] num` |
| 9 | 617–880 | 大整数 | `big_int_equal`(617)…`big_int_great_s`(862)；`big_int_neg/min_u/min_s/max_u/max_s/clz/ctz`(874–880) **空体桩** | `Arr[U64]` 逐 limb |
| 10 | 883–904 | 泛型随机 | `random(type: @Type) @Type`(883) | `size(type)` + `array` + `raw_cast(<@Type>,…)` |
| 11 | 906–972 | 数学 | `log10(U64)`(906) | 20 级 `if/elif` 手写 |
| 12 | 974–1108 | 数组算子与排序 | `binary == / !=`(`Arr[@Any]`)(974/997)、`dot contains`(954)、`dot in`(968)、`dot high`(1001)、`quick_sort($arr)`(1007)、`sort($arr)`(1084) | `quick_sort` 是**嵌套 def + 闭包**（`partition`/`swap`/`inc`，全捕获外层 `$arr`） |
| 13 | 1098–1340 | 字符串 | `binary + Str`(1098)、`+=`(1104)、`str(Str/Bool/Uint/Sint/Ptr/Char/Arr[@Type])`(1110/1116/1128/1152/1236/1270/1276)、`hex(Int,Int)`(1182)、`bin`(1209)、`int(Str)`(1240)、`print(Arr[@Type])`(1299)、`print(@Any)`(1305)、`index_read/index_write`(1312/1318)、`binary == / != Str`(1324/1330)、`dot high(Str)`(1336)、`dot len(Str)`(1092) | 8 个 `str` 重载 + 2 个 `print` 重载 |
| 14 | 1343–1461 | 时间单位与算子 | `#NANOSECOND…#WEEK`(1343–1350)、`dot sec/ms/us/ns/min/hour/day`(1352–1388)、`binary + - *`(`Time`)(1394/1400/1406)、`+= -=`(1412/1418)、`< > <= >=`(1424–1442)、`exit()`(1450)、`dot write(File,Str)`(1456) | `#MINUTE = #SECOND * 60` 这类**常量引用常量** |
| 15 | 1464–1503 | 随机实现 | `xorshift(Int)`(1465)、`global_seed`(1475)、`randomize_seed`(1477)、`dot next($s: Seed)`(1481)、`random()`(1486)、`random(max)`(1491)、`sample(Arr[@Any])`(1497) | 见 §B |

---

## A2. 语言构造 × 我们的支持性（**实测**）

口径（重要）：判据 = `build/simplexc_cli.exe --src <片段> --out <bin>` 的退出码与报错文本；
**每个构造都放在「被根块调用的 def」里**，否则整份 def 会被丢掉、报不出错（反证：`array` 这样写会点名报错，说明调用链确实进了 lowering）。
产物字节数**不能**当判据：41 份产物里 **39 份逐字节相同**（只有带 `store` 副作用的两份不同）——
没副作用的片段会被 DCE 成同一份代码；`entry` 落在 `.entry` sidecar 里。支持性只看 CLI 是否点名报错。

命令：`python work/tmp/stdlib/constructs.py > work/tmp/stdlib/constructs.log`（逐条原始输出 `work/tmp/stdlib/constructs/<名字>.log`，汇总 `constructs.tsv`）。

### 支持（20/46）

| 构造 | 前缀里的用例 | 实测 |
|---|---|---|
| 类型化常量 / 常量引用常量 | `const #INT_HIGH = Int 0x…`、`#MINUTE = #SECOND * 60` | PASS（`const_typed`、`const_ref_const`） |
| 函数内局部 `const #N` | `allocate_raw` 的 `#MEMORY_CHUNK` | PASS（`const_local_in_def`） |
| 类型别名 | `type Str Arr[Char]`、`type Time U64` | PASS（`type_alias`） |
| 单行 `Enum[`（含 256 员） | `SimCommand Enum[…]` 一类 | PASS（`enum_many`） |
| 枚举成员参与运算 | `a + U8 ch_a - 10`（`hex`） | PASS（`enum_member_arith`） |
| `binary` 算子单元（非 `$`） | `binary + (a: Ptr, b: AnyInt)` | PASS（`binary_unit`） |
| `dot` 算子单元 | `dot len(input: Str) Int` | PASS（`dot_unit`） |
| `index_read`/`index_write` 单元 | 1312/1318 | PASS（`index_unit` 的**声明**部分） |
| `call_conv x86_64 windows_x64(…)` | 243–249 | PASS（`call_conv`） |
| `raw_cast(<T>, x)` | `raw_cast(<Arr[U64]>, …)` | PASS（`raw_cast`） |
| `store`/`load(<T>, p)` | `set_length`/`get_length` | PASS（`store_load`） |
| 同名重载（按元数） | `exit()` / `exit(code: Int)` | PASS（`overload_arity`） |
| 同名重载（按类型） | `str(Uint)` / `str(Sint)` | PASS（`overload_type`） |
| 完全相同的 def 重复定义 | `assert` 158 / 515 | PASS（`dup_identical_def`） |
| 三目 `?:` | `int()` 的 `negative ? 1 : 0` | PASS（`ternary`） |
| 字面量上的 `dot`（`3.ms()`） | `sleep(1.ms())` 一类 | PASS（`dot_on_literal`） |
| `switch id`（空体） | `ui_set_*` 一族 | PASS（`switch_empty_body`） |
| `#if in_scope("x"){…}` | 语料与 base.si | PASS（`in_scope_if`） |
| `if/elif/else` 链 | `log10` | PASS（`elif_chain`） |
| `标签: while` + `break 标签` | 语料 `run_sim`（前缀里没有） | PASS（`labeled_break`） |

### 不支持（19/46，都是构造本身的锅）

| 构造 | 前缀里的位置 | 实测报错（原文见 constructs/<名字>.log） |
|---|---|---|
| `$` 可变参数（`$a`/`$s`/`$arr`） | 172/180、`dot next($s)`、`quick_sort($arr)` | 词法层：`不认识的字符 '$'`（`dollar_param`/`binary_mut_unit`/`nested_def_closure`） |
| asm 里的 `$name` 操作数 | `get_cycle_count` 的 `mov $result, rax` | 同上（`asm_block`） |
| `asm aarch64 { … }` | 45（`print_using_stack`）、143（`breakpoint`） | 语法错误：`语句位置出现没有副作用的表达式（CastExpr）`（`asm_aarch64`） |
| `@Any` 编译期类型参数 | `print(@Any)`、`assert`… | `UNSUPPORTED: 编译期类型参数 Any`（`generic_any`、`arr_any_param`） |
| `@Type` + `@Type` 返回 + `raw_cast(<@Type>,…)` | `dot push/pop/in/contains`、1280 的 `str(Arr[@Type])` | 同上（`generic_type`、`raw_generic_dispatch`） |
| `@Size` + `size(x)` | `hex(num: @Size)` | 同上（`generic_size`、`builtin_size` → `编译期类型参数 Size`） |
| `[T] expr` 方括号转换 | `var result = [U8] input`、`(Arr[U8] text)`、`Arr[U8] a` | 语法错误：`语句位置出现没有副作用的表达式（IdentExpr）`（`bracket_conv`） |
| `extern windows_x64 kernel32` + 命名空间调用 | 251 起全部系统层 | `UNSUPPORTED: 未定义的函数 kernel32`（`extern_plain_call`） |
| `.'引号成员名'` | `kernel32.'ExitProcess'`、`.'_write'`… | 语法错误：`这里要 成员名，看到 字符串字面量`（`extern_quoted_call`） |
| **多行** `Enum[`（换行紧跟 `[`） | `type Char Enum[`(17) | `编译失败：枚举声明看不懂`（`enum_multiline`） |
| 裸块 `{ … }` | 前缀里没有（语料 12 份有） | `UNSUPPORTED: 语句形态 ?`（`bare_block`） |
| `Char` 出现在 `index_write` 形参位 | 1318 | `UNSUPPORTED: 类型 Char`（`index_unit` 第 4 行 —— 两份 `index_*` 声明里只有这一份被拒） |

### 口径混淆、不能算证据（5/46）

`builtin_type_of_element` / `builtin_grow_shrink` / `builtin_join` / `empty_def_body` / `dot_method_len`：
我给的实参是 `Str 0` / `"abc"` 之类的转换表达式，CLI 先报 `f() 的重载解析拿不到参数类型`，
**def 体没被走到** → 这几个用例对本报告无用（内建那 3 条改用 `g1/g2/g3.si` 单独重测，见 §A3）。

### 整幅：前缀本体

```
$ ./build/simplexc_cli.exe --src work/tmp/stdlib/constructs/_prefix_body.si --out …   # 前缀原文 + 最小 run_sim 骨架
语法错误：第 172 行 第 12 列：不认识的字符 '$'      # 就是 `binary += ($a: Ptr, b: AnyInt) None {`
```

即：**第一个挡死的构造在 172 行**（§A2 的 `$`）。修掉 `$` 之后的下一个拦路面**未测**（本轮没往下走）。

---

## A3. 内建 / 宏清单

「只服务前缀自身」的判据 = 在 98 份 `simplexc/corpus/src/*.si` 里出现份数（`grep -l`，口径与 status 一致）。

| 内建 | 前缀用处（行） | 98 份语料用它的份数 | 结论 |
|---|---|---|---|
| `array(<T>, n)` | 38 处（`str(Uint)`、`hex`、`random(@Type)`、`quick_sort` 的栈） | **16** | 两边都要；实测 `未定义的函数 array` |
| `size(x)` | 3 处（`hex(@Size)`、`random(@Type)`） | **24**（`size(value)` / `size(size)`；`grep -lE` 匹配「非下划线开头 + `size(`」） | 两边都要；挂在 `@Size` 缺口上 |
| `grow` | 2（`dot push/cstring`） | 0 | **只服务前缀自身**；实测 `未定义的函数 grow`（`g1.si`） |
| `shrink` | 1（`dot pop`） | 0 | 只服务前缀自身；同上文件里 `grow` 先挡，`shrink` 未单独验 |
| `join` | 1（`binary + Str`） | 0 | 只服务前缀自身；实测 `未定义的函数 join`（`g2.si`） |
| `empty_array_address()` | 2（`allocate_raw`、`__shrink`） | 0 | 只服务前缀自身；实测 `未定义的函数 empty_array_address` |
| `type_of_element(a)` | 1（`quick_sort`） | 0 | 只服务前缀自身；**未单独实测**（同处 `array` 先挡，`g3.si` 报的是 `array`） |
| `clz` / `ctz` / `ror` / `min` / `max` | `clz`(325)、`min/max`(1184)… | `clz` 2 份、`ror` 2 份、`min/max` 98 份 | 已有（`clz`/`ror` 是语料侧已知缺口，见 status） |
| `in_scope("…")`（`#if` 宏） | 0（前缀不用） | 98 | 已支持（§A2） |

结论：**7 个缺口里只有 `array` 与 `size` 同时服务语料**；`grow/shrink/join/empty_array_address/type_of_element` **只服务前缀自身**（语料里 0 份），
所以它们属于「路线 A 的账」，路线 B 不该为它们付成本。

---

## B. `random` 的语义实测

器材：`work/tmp/stdlib/probe_stdlib.py`。片段 = `work/tmp/ul7x/base.si`（游戏自己的 chip 源）+ 探针，
把探针调用插在**每 tick 的 cycle 内循环体第一行**；参考编译 → `.ref.bin`；宿主 3 tick：
- `print(...)` 路径 → `.jsonl` 里 `_write` 记录；
- 精确 64 位走 **`store(ui_buffer + 20000 + k*8, U64 random(...))`** → `.pages/t000N_<页>.bin`。
（`ui_buffer` = `2416633380864`，base.si 里就有；这样绕开 `str()`/`output()` 的十进制转换。）

参考源（前缀 1464–1503）：

```
def xorshift(input: Int) Int {
    var x = input
    x ^= x << 12
    x ^= x >> 25
    x ^= x >> 27
    return x * 0x2545F4914F6CDD1        // ← 15 位十六进制
}
var global_seed = Seed 1
dot next($s: Seed) Int { s = Seed xorshift(Int s)  return Int s }
def random() Int { var result = $global_seed.next()  return result }
def random(max: Int) Int { var result = random()  return Int (U64 result % U64 max) }
```

### B1. 算法与常数（已验，精确 64 位）

`work/tmp/stdlib/probe_stdlib.py store`（8 次迭代 × 4 个调用），从落盘页里读出：

| 列 | 观测（前 4 个，16 进制） | 与 Python 复算的关系 |
|---|---|---|
| `store(… , U64 random())` | `4848f09881d3ddd1` `c75b675c97dc9b97` `5c326bdbf2e5e254` `14c1a47db4f02de2` | = `state_{4k+1}` |
| `U64 random(-1)` | `7d4e885c71bb86a6` `959074466ddc899f` `c065d8a88e9d1f18` `8740ca22d671bfed` | = `state_{4k+4} % (2^64-1)` |
| `U64 random(2)` | `0 1 1 0` | = `state_{4k+3} % 2` |

复算脚本（含常数对比）产出：**15 位常数 `0x02545F4914F6CDD1` → 三条列全 True；教科书 16 位 `0x2545F4914F6CDD1D` → 三条全 False**。

```
state_0 = 1
state_{n+1} = ( (x ^ (x<<12) ^ (x>>25) ^ (x>>27)) * 0x02545F4914F6CDD1 )  mod 2^64 ,  x = state_n
random()   = state_{n+1}                      （返回新 state，全 64 位）
random(m)  = U64(state_{n+1}) % U64(m)
```

> 注意：**那个常数少一位**（真实 xorshift64* 是 `0x2545F4914F6CDD1D`）。实现路线 B 时照抄 `0x02545F4914F6CDD1`，不要「顺手修正」。

### B2. 种子：来源、初值、是否每 tick 变（已验）

- **初值 1**：第一次 `random()` 的输出与 `state_1`（由 `state_0 = 1` 推出）**逐位相同** → 起步种子就是 `var global_seed = Seed 1`。
- **跨 tick 不重置**：`seq`/`mod`/`store` 三份片段在 tick 1/2/3 上的输出依次是 `state_1, state_2, state_3, state_4…`
  （`mod` 的观测 `819601 | 235340`（tick1）`683650`（tick2）`907046`（tick3）＝ `state_1..4 % 10^6`）。
  链连续、无重复、无跳变 → 种子是**程序自己的内存**，随调用单调推进；**没有任何每 tick 的重播种**。
- **游戏/前缀自己不会播种**：98 份语料里 **0 份**调用 `randomize_seed`（全仓 .si 里出现它的只有：前缀本体及其 hook/snapshot 副本、`work/tmp/clifail/prefix_only.si`、`work/tmp/prefix/*.si`、本轮 `work/tmp/stdlib/*`），
  且第一次输出恰等于 `state_1` —— 若游戏在启动时播过种，这个等式不可能成立。
- **地址未验**：宿主只落「源码算出的 spec 区」的页，程序自有数据段不在其中 —— 我在 3 个 tick 的全部落盘页里搜 `state_1..state_4`
  0 命中，所以**种子的绝对地址没测出来**，标「未验」。已知的只是：它不是游戏地址表里的任何一项（`spec.regions()` 只由源码地址表与 `memory_copy/clear` 实参决定）。
- `randomize_seed()` = `global_seed = Seed get_cycle_count()`，而 `get_cycle_count()` 读 **rdtsc**（x86_64 asm）→ 它给的是 TSC，**不是游戏内存地址**。
  实测（`reseed` 片段，每 tick 打 3 个值）：`819601 | 872770`（tick1）`464423 | 588503`（tick2）`309574 | 872774`（tick3）
  —— 播种后立刻取的数**不再落在 `state` 链上**，且两次播种给出不同的值（`872770` vs `872774` 形似但非同一序列）。

### B3. 范围与边界（已验）

| 情形 | 观测 | 结论 |
|---|---|---|
| `random(m)`, `m>0` | `state % m`（`mod`、`store` 两路都对上） | `[0, m)`，**不含端点** |
| `random(1)` | 恒 `0`（`store` 页里 8 次全 0） | `[0,1)` |
| `random(-1)` | `state % (2^64-1)`（`store` 页逐位对上） | 负数先按 U64 重解释 |
| `random(0)` | **返回被除数**：`zero_store` 页里依次得到 `state_1 state_3 state_5 state_7`（与夹在中间的 `random(3)` 输出 `1 2 2 0`＝`state_2/4/6/8 % 3` 完全自洽） | **不 trap、不返回 0**，`x % 0 == x`。宿主的 VEH 只会 `TerminateProcess`，不会吞异常 → 这不是宿主兜的 |
| `random(2)`、`random(3)`、`random(4)` | 与 `% m` 逐位一致 | — |
| `random()`（无参） | 语料 0 份用（32 份全是 `random(<整型字面量>)`） | 但确实是合法重载 |

### B4. 未验 / 别当成结论

- 种子的**具体地址**（见 B2）——未验。
- `random(@Type)`（泛型重载，883）与 `sample(Arr[@Any])`（1497）：没被语料用到，本轮**没测**。
- 观测到一个**旁支现象**（与 random 无关，记下来待查）：`print()` 路径上 >8 位十进制数被截成**前 8 个字符**
  （例：`random()` 打到 `_write` 的记录是 `n=19, data=b'52086775\x00…'`）。成因未定（参考 `str()` 的数组长度 / 宿主 `_write` 桩的取字节口径，两个候选），
  §B 的结论**不走这条路径**，所以不受影响。别拿 `print(长数字)` 当判据。

### B5. 明确回答：只做路线 B，`random` 能单点实现吗？

**能，而且是 7 个内建里最便宜的一个。** 前置：

1. **一个 8 字节程序全局**（`global_seed`）+ 读写它 —— 我们已经有顶层 `var`（语料侧的全局），**不需要 arena、不需要 `array`/`size`/`@Type`**（`random(@Type)` 那条重载语料 0 份）。
2. **照抄 15 位常数** `0x02545F4914F6CDD1` 与移位 12/25/27；`random()` 返回**新 state**（不是旧 state，也不做「先取输出再推进」）。
3. `random(m) = U64(state) % U64(m)` 的**除零口径**要与参考一致（`m==0` → 返回被除数）。
4. 种子初值 1、**不要**在 tick 边界重置（我们 lowering 只要别把 `global_seed` 当每 tick 重初始化的东西）。
5. 顺序敏感：调用点每多一次/少一次 `random()`，整条链全错 → 判据必须用「同源码两边逐 tick 状态等价」而不是抽样。

`randomize_seed()`/`get_cycle_count()` 是可选件：语料 0 份调用，但它依赖 `asm x86_64` 的 `rdtsc`（前缀 565）与 asm 里的 `$result` —— 要真做，先解 `$` 那类缺口。

### B6. `get_time` / `sleep` 简答（与我们的 lowering 一致）

参考（553–551）：`get_time = Time((GetSystemTimePreciseAsFileTime() - 116444736000000000) * 100)`；`sleep(d) = Sleep(d / 1000000)`。
我们 `simplexc/lower/lower.cc`：`kFileTimeUnix = 116444736000000000`(:58)、`kNsPerMs = 1000000`(:59)、
`body_get_time()` = `(ft - kFileTimeUnix) * 100`(:2050)、`body_sleep()` = `udiv d, kNsPerMs` 后截 u32(:2056) → **逐项相同，无出入**。
实测（`time` 片段）：宿主桩每次调用 +10 ms，打印值 `2000000 / 6000000 / 9000000` —— 都是 `10^6` 的整数倍 ⇒ 单位 = **ns**、两次调用之间的步长 = 1 ms，与上面公式自洽。

---

## 复现命令

```bash
python work/tmp/stdlib/probe_stdlib.py mod raw store zero zero_store reseed time   # B 的原始输出（各 .jsonl/.pages/.compile.log）
python work/tmp/stdlib/constructs.py > work/tmp/stdlib/constructs.log              # A2 的 44 份片段 + 前缀本体
python work/tmp/stdlib/constructs.py                                               # （再次运行会覆盖同一批 .si/.log/.bin）
```

## 遗留风险

1. 支持性结论绑定 `90b9b6c` 附近的快照；`frontend/**`、`lower/**` 正在被改 —— 重测前别引用具体 PASS/FAIL。
2. 前缀本体只测到「第 172 行首次失败」，**第 2 个拦路面未知** → 路线 A 的成本目前是下界。
3. §B4 的「>8 位数字被截断」若真是参考 `str()` 的行为，会影响字符串/插值那一摊（U-L3）；本轮没定性。
4. `random(0)` 的返回被除数只在**参考产物 + 宿主**里验过（宿主 VEH 不吞异常，所以不是宿主兜的），**没在游戏内验**。

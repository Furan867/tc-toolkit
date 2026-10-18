# stdlib 前缀（路线 A）可行性摸底

日期：2026-10-18。只读产品代码（另一个 worker 正在改 `simplexc/lower/**`）。
摸底对象 = `simplexc/corpus/stdlib_prefix.si`（1503 行 / 27656 B，原 27711 B 含 CRLF）。
本文件只回答一件事：**这个前缀现在能不能过我们的前端；过不了的话卡在哪几类构造。**

## 结论（三行）

1. **不能过，而且当前 CLI 还没走到 lowering 就死了两次**：先 `$`（词法，第 172 行）→ 再多行 `Enum[`（声明层，第 17 行）。
2. 走 DSL 判据（`ast_dump --all`，绕过上面两道门槛只看语法）后，**121 个顶层单元里 103 个能解析、18 个失败，失败只落在 3 个语法类**：
   `asm` 块（5 单元）· `kernel32.'Name'(…)` 外部调用（8 单元）· `[T] expr` 方括号转换（5 单元）。
3. **缺口不只在前端**：能解析 ≠ 能编。前缀用到 7 个我们 lowering 没有的内建/宏
   （`array` / `grow` / `shrink` / `join` / `empty_array_address` / `type_of_element` / `size`），
   16 个单元的签名带 `@Type`/`@Any`/`@Size` 编译期类型参数（10 个 def + 6 个算子单元，lowering 直接 `Unsupported`），
   还有 39 个顶层算子单元（`binary +` / `dot push` / `index_read`）**在声明层完全不可见**
   —— 它们是前缀的泛型/重载面，也是路线 A 最大的一块。

## 一、判据原始输出

### 1. 完整前缀，一次干净跑（判据 ①）

```
$ ./build/simplexc_cli.exe --src simplexc/corpus/stdlib_prefix.si --out work/tmp/prefix/prefix.bin
词法错误：第 172 行 第 12 列：第 172 行：不认识的字符 '$'
EXIT=1
```

卡点原文（第 172 行）：`binary += ($a: Ptr, b: AnyInt) None {`
整个文件死在构造期（`tokenize(raw_text)` 在 `Frontend` 构造里做），**所以这一条拦住了后缀全部**：
`ast_dump --all`、CLI、`--list-unsupported` 都拿不到任何后续信息（`ast_dump --decls` 那一类不分词的入口照旧可用）。

### 2. 把 `$` 删掉后再跑（**摸底副本**，不是产品改动）

```
$ sed 's/\$//g' simplexc/corpus/stdlib_prefix.si > work/tmp/prefix/prefix_nodollar.si   # 1503 行不变
$ ./build/simplexc_cli.exe --src work/tmp/prefix/prefix_nodollar.si --out work/tmp/prefix/prefix_nodollar.bin
编译失败：第 17 行：枚举声明看不懂：type Char Enum[
EXIT=1
```

第 17 行是 `type Char Enum[` —— 枚举体跨 17–36 行，声明扫描只把 decl 记成 `17-17`
（`hi` 不跨 `[`），于是 `module_build::scan_type` 拿到的是半截文本。

### 3. 再把枚举那 20 行删掉，看到第三道（同样只是摸底副本）

```
$ sed '17,36d' work/tmp/prefix/prefix_nodollar.si > work/tmp/prefix/prefix_nodollar_noenum.si
$ ./build/simplexc_cli.exe --src work/tmp/prefix/prefix_nodollar_noenum.si --out work/tmp/prefix/noenum.bin
编译失败：没有顶层语句块（程序根）
EXIT=1
```

**这是结构性的，不是缺口**：`build_module` 只解析**可达** def（`fe.live`），而 `live` 的根来自
label / 顶层 `var`/`const` 行里的调用。前缀里没有 label，`live` 是空集 → 没有任何 def 被解析。
→ 路线 A 的形态只能是「**前缀 + 用户源码**」这一个整体（用户源码带 `run_sim:` 根）；
单独喂前缀给 CLI 永远看不到 lowering 层。本次摸底因此用 `ast_dump --all`（逐 def 解析）+ 探针给 lowering 层取证。

### 4. `ast_dump --all` 逐单元结果（判据 ②的主表）

```
$ ./build/ast_dump.exe --src work/tmp/prefix/prefix_nodollar.si --all --quiet
（全表落盘 work/tmp/prefix/all_flat.tsv —— 下面是扁平化副本的同一跑，见「口径与噪声」）
共 121 个单元：103 OK / 18 失败
print_using_stack  def   45  141  失败 第 101 行：语句位置出现没有副作用的表达式（CastExpr）
breakpoint         def  143  155  失败 第 145 行：语句位置出现没有副作用的表达式（CastExpr）
memory_copy        def  186  198  失败 第 188 行：语句位置出现没有副作用的表达式（CastExpr）
memory_clear       def  200  212  失败 第 202 行：语句位置出现没有副作用的表达式（CastExpr）
exit               def  253  255  失败 第 254 行：这里要 成员名，看到 字符串字面量
memory_reserve     def  257  265  失败 第 259 行：这里要 成员名，看到 字符串字面量
memory_commit      def  267  276  失败 第 269 行：这里要 成员名，看到 字符串字面量
memory_free        def  278  286  失败 第 280 行：这里要 成员名，看到 字符串字面量
thread_exit        def  289  293  失败 第 291 行：这里要 成员名，看到 字符串字面量
_op507（dot cstring）def 507 513 失败 第 509 行：语句位置出现没有副作用的表达式（IdentExpr）
output             def  536  544  失败 第 542 行：这里要 成员名，看到 字符串字面量
sleep              def  546  551  失败 第 549 行：这里要 成员名，看到 字符串字面量
get_time           def  553  560  失败 第 555 行：这里要 成员名，看到 字符串字面量
get_cycle_count    def  565  575  失败 第 567 行：语句位置出现没有副作用的表达式（CastExpr）
big_int_add        def  647  664  失败 第 656 行：这里要 ')'（配对第 656 行的 '('），看到 'a'
big_int_xor        def  762  775  失败 第 770 行：这里要 ')'（配对第 770 行的 '('），看到 'a'
str(Char)          def 1270 1274  失败 第 1274 行：表达式里出现意外的 '}'
_op1456（dot write）def 1456 1461  失败 第 1458 行：语句位置出现没有副作用的表达式（IdentExpr）
```

## 二、缺口分类（判据 ②）

计数口径：**单元数** = 首个错误落在该类里的单元数（`ast_dump --all` 给的就是这个）；
**全文件处数** = 按构造 grep 全文件的真实出现处（不是"多算一次同类错误"，是以后修错了要知道还有几处）。

### 类 A：`$` 前缀名（词法层，**整个文件的门槛**）

| 项 | 值 |
|---|---|
| 计数 | 23 处 / **13 个顶层单元** |
| 出现 | `binary += ($a: Ptr, b: AnyInt) None {`（L172、L180）· `def __append(arena_header: Ptr, $a: Ptr, b: Ptr, element_size: Int) Ptr {`（L451）· `dot push($array: Arr[@Type], value: @Type) None {`（L483）· `dot pop($array: Arr[@Type]) @Type {`（L491）· `def get_cycle_count() Int {` 里的 `mov $result, rax`（L572，在 asm 体里）· `def quick_sort($arr: Arr[@Any]) None {`（L1007，另 9 处在体内）· `def sort($arr: Arr[@Any])`（L1084、L1086）· `binary += ($a: Str, …)`（L1104）· `binary += ($a: Time, …)`（L1412）· `binary -= ($a: Time, …)`（L1418）· `dot next($s: Seed) Int {`（L1481）· `$global_seed.next()`（L1487，**全局读**，另一种含义） |
| 原文 | `$a: Ptr` = 可变参数（被调方写回）；`$global_seed` = 引用全局名 |
| 要做什么 | lexer 认 `'$' ident`（现在 `in_ops1` 没有 `$`）→ AST `Param` 加一个标记 → lowering 做写回（调用点传地址）。前端约 30 行；**lowering 是真活**（`lower/README.md` 已把「`$` 写回」列为语义保真映射项）。`$global` 读是另一档：把全局名解析成引用，不是参数写回。 |

### 类 B：`asm <target> { … }` 内联汇编

| 项 | 值 |
|---|---|
| 计数 | 6 个**有效**块 / **5 个 def**（另 L50 那块在 `/* … */` 注释里，不算） |
| 出现 | `asm aarch64 {`（L101，`print_using_stack`；L151，`breakpoint`）· `asm x86_64 {`（L145 `breakpoint`；L188 `memory_copy`；L202 `memory_clear`；L567 `get_cycle_count`） |
| 原文 | `asm x86_64 {` / `asm aarch64 {` / `asm x86_64 windows {`，体里是 `mov/udiv/msub/rdtsc/rep movsb/stosb/svc 0x80/brk` |
| 要做什么 | 前端加 AsmStmt（体按原始 token 存），lowering 用 LLVM inline asm（LLVM-C 有 `LLVMGetInlineAsm`）+ 寄存器/约束。4 块 x86_64 里 3 块是真正的指令（`rdtsc`/`rep movsb`/除零）；**2 块 aarch64 可以直接"解析后跳过"**（我们的目标是 x86_64-pc-windows-gnu）。这是唯一一类真正需要**新代码生成能力**的。前端小、lowering 中。 |

### 类 C：`dll.'Symbol'(args)` 外部调用

| 项 | 值 |
|---|---|
| 计数 | 8 个调用点 / **8 个 def** |
| 出现 | `kernel32.'ExitProcess'(code)`（L254）· `Ptr kernel32.'_virtual_alloc'(…)`（L259）· L269 `_virtual_alloc` · L280 `_virtual_free` · `kernel32.'ExitThread'(0)`（L291）· `kernel32.'_write'(text, length)`（L542）· `kernel32.'Sleep'(milliseconds)`（L549）· `Int kernel32.'_get_system_time_precise_as_file_time'()`（L555） |
| 原文（报错处） | `kernel32.'ExitProcess'(code)` → `这里要 成员名，看到 字符串字面量` —— 成员名写成**字符串字面量**，parser 不接受 |
| 要做什么 | parser 一处（`.名` 允许字符串字面量成员，AST `Member` 存字符串），并把 `extern windows_x64 kernel32 { … }`（L251）与 `call_conv x86_64 windows_x64 (…)`（L243–249）解成签名表；现在 lowering 的 `get_extern()` 是**按名字硬编码**的。注意 L542 的注释：`WriteFile` 5 参数要栈传参，说明这套 ABI 不能只做 4 寄存器。前端小、lowering 中。 |

### 类 D：`[T] expr` 方括号前缀转换（含 `T [expr]`）

| 项 | 值 |
|---|---|
| 计数 | 6 处 / **5 个单元**（3 处是隐藏在同一单元首错之后的） |
| 出现 | `var result = [U8] input`（L509，`dot cstring`）· `let a_val = ([U64] a)[i]`（L656、L657，`big_int_add`）· `result[i] = ([U64] a)[i] ^ ([U64] b)[i]`（L770，`big_int_xor`）· `return Str [input]`（L1273，`str(Char)`）· `var data = [U8] text`（L1458，`dot write`） |
| 原文 | 三种拼写同一个构造：`[U8] input`、`([U64] a)[i]`、`Str [input]` |
| 最小复现 | `work/tmp/prefix/probe_forms.si`：`p1 var x = [U8] a` → `语句位置出现没有副作用的表达式（IdentExpr）`；`p2 ([U64] a)[0]` → `这里要 ')'…看到 'a'`；`p3 return Str [input]` → `表达式里出现意外的 '}'`。**注意 `Arr[U8] a`（字母拼写，非方括号）是 OK 的**（同一探针 p7 通过）|
| 要做什么 | parser 的表达式前缀一处：`[` 后面是类型名时按转换解析（并把 `T [expr]` 同一处理）。**lowering 不用动**（`Arr[T]`/`Str`/`Ptr` 之间的转换已实现）。前端小（~20 行）。 |

### 类 E：`<T> expr` 尖括号转换（值位置）

| 项 | 值 |
|---|---|
| 计数 | 1 处 / 1 个单元 |
| 出现 | `return <Cstring> result`（L511，`dot cstring`，被类 D 的首错挡住） |
| 最小复现 | `probe_forms.si` p4：`return <Cstring> a` → `表达式里出现意外的 '<'` |
| 要做什么 | parser 已有「调用里的 `<T>` 类型实参」（`array(<U8>, n)`、`load(<U16>, p)` 都在用），只是**表达式位置**的 `<T>` 没接。和类 D 一样是 parser 一处。前端小。 |

### 类 F：`type X Y` 类型别名（6 条，声明层）

| 项 | 值 |
|---|---|
| 计数 | 6 条 |
| 出现 | `type Time U64`（L38）· `type Seed Int`（L39）· `type File Int`（L40）· `type Ptr U64`（L41）· `type Str Arr[Char]`（L42）· `type Cstring Arr[Char]`（L43） |
| 说明 | `match_type` 只认 `Enum`/`Struct`，别名落到「未支持的顶层构造」清单（19 条里有 6 条） |
| 要做什么 | `decls.cc` 的 `match_type` + `module_build::scan_type` 各加别名分支（记进 `type_names`，lowering 里做别名→真类型）。**这条很关键**：我们的 lowering 现在把 `Str` 当独立 `TyKind`，别名语义通了才能说「更多类型进得来」。小。 |

### 类 G：多行 `Enum[…]`（声明层，**第二个 CLI 门槛**）

| 项 | 值 |
|---|---|
| 计数 | 1 条（`Char`，跨 L17–36，256 个成员） |
| 出现 | `type Char Enum[` L17 … L36 `]` |
| 说明 | 声明扫描把 `hi` 记成 17 → `scan_type` 拿到半截文本 → `CompileFail("枚举声明看不懂")`。同时 L18/19/23/27/31/34 六行被记成「未支持」（**记账噪声**，见下节） |
| 要做什么 | 声明扫描里给 `[...]` 补一个「括号内不结束声明」的口径（对 `Enum[` 到 `]` 的并 `hi`）。小（~10 行），但它是路线 A 走到 lowering 的必经之路。 |

### 类 H：39 个顶层算子单元（`binary`/`dot`/`index_*`）——**声明层完全不可见**

| 项 | 值 |
|---|---|
| 计数 | **39 个单元**（`binary …` 19 · `dot …` 18 · `index_read`/`index_write` 2） |
| 出现 | `binary + (a: Ptr, b: AnyInt) Ptr {`（L168）· `dot push($array: Arr[@Type], value: @Type) None {`（L483）· `dot len(input: Str) Int {`（L1092）· `index_write (input: Str, index: Int, value: Char) None {`（L1318）· `binary + (a: Str, b: Str) Str {`（L1098）… 全表见 `grep -n '^\(binary\|dot\|index_read\|index_write\)'` |
| 为什么不可见 | 探针 `probe_op.si`（只有 `binary + (…) { … }`）→ `--decls` 报「未支持的顶层构造：无、声明 0 条」。原因：unsupported 判定发生在**花括号配平之后**，该行结尾 `{` 已把 `depth` 抬到 1，于是既不进 decls 也不进 unsupported —— 静默丢失。 |
| 要做什么 | ① decls 层认算子头（`kind="op"` + 操作符/方法名）；② AST/lowering 需要**用户定义算子/方法的分派表**（`+` 用在 `Str`/`Arr`/`Time` 上要落到前缀 def，而不是 lowering 里硬编码的 `Str +`）。这一条与「泛型 `Arr[@Any]` + 重载」是同一块机制 —— **路线 A 最大的一块，且大头在 lowering。** |

## 三、追问题：只有前端，还是也牵出 lowering / runtime？

**答：两边都牵，而且 lowering/runtime 那半边更重。** 证据（前缀用到、我们 lowering 里零命中）：

| 前缀用到 | 位置 | 我们这边 | 判据 |
|---|---|---|---|
| `array(<T>, n)` | 38 处（如 L587、L620、L650、L1051） | `UNSUPPORTED: 未定义的函数 array` | 探针 `probe_array.si` 实测（CLI 原始输出见下）；`docs/notes/2026-10-18-m5-coverage.md` 里 084/096/098 三份语料同一个错 |
| `grow(a, n)` / `shrink(a, n)` | L486、L502、L510 | 零命中 | 前缀与 **98 份语料全都没有 `def grow`/`def join`/`def array`** → 参考实现里是内建/宏；我们 lowering 里没有 |
| `join(a, b)` | L1100（`binary + (Str, Str)`） | 零命中 | 同上 |
| `empty_array_address()` | L323、L418 | 零命中 | 同上 |
| `type_of_element(arr)` | L1051（`quick_sort`：`array(type_of_element(arr), high + 1)`） | 零命中；`@` 类型值一律 `Unsupported` | lowering `lookup_name`：`@` 开头的名字 → `Unsupported("编译期类型参数")` |
| `size(x)` 运行期 | L581 `const #LEN = (size(num) + 3) / 4`、L884 `const #SIZE = size(type)` | 运行期 `size()` → `Unsupported("size() 只支持编译期形态")`；常量求值器只认 `raw_cast`，别的调用 → `Unsupported("常量求值遇到函数调用")` | lowering L1666、L2154 |
| `@Type`/`@Any`/`@Size` 参数 | 16 个单元（`print_using_stack`、`hex(num: @Size)`、`random(type: @Type)`、`str(array: Arr[@Type])`、`print`×2、`sample`、`quick_sort`、`sort`、`binary ==/!=`、`dot push/pop/contains/in/high`） | 类型引用里 `@` → `Unsupported("编译期类型参数")`；`Arr[T]` 本身支持 | lowering L809、L833 |
| 重载 | `str`×5、`print`×2、`exit`×2、`random`×2、`hex`×2、`assert`×2、`big_int_*` | `defs_` 重载解析「拿不到参数类型」→ `Unsupported` | lowering L1670 |
| `asm` | 见类 B | 无 | —— |
| 全局 `var data` / `var global_seed` + `$global_seed` | L1458、L1475、L1487 | 前缀全局是**白名单硬编码**（`prefix_globals()`） | lowering L736 |

探针 `probe_array.si`（`run_sim: while true { var a = array(<U8>, 4) … }`）原始输出：

```
UNSUPPORTED: 未定义的函数 array @ 第 2 行
EXIT=1
```

**不属于前端、也不属于 lowering 的一条**：路线 A 的输入形态必须是「前缀 + 用户源码」整体（见一.3）——
单跑前缀没有程序根，`build_module` 一个 def 都不解析。

## 四、口径与噪声（别把它们混进分类）

1. **只跑、不改**：产品代码与 `simplexc/corpus/stdlib_prefix.si` 一个字没动。摸底副本都在 `work/tmp/prefix/`：
   `prefix_nodollar.si`（删 `$`，1503 行不变）· `prefix_flat.si`（在 `$` 之外还把 39 个算子头改写成 `def _op<原行号> (`，行号不变）· `prefix_nodollar_noenum.si`（再删 L17–36）· `flatten.sed` · `probe_*.si` · `all_flat.tsv`（`--all` 原始表）· `at_units.tsv`。
2. **算子单元必须扁平化才摸得到**：原文件里它们不进 decls、不进 unsupported、也不进 `--all`（只遍历 `kind=="def"`/`label`）→ `prefix_flat.si` 是唯一能看到这 39 个单元体的视图。**`_op507`/`_op1456` 就是靠它才露出来的。**
3. **多行 `Enum[…]` 的记账噪声**：`--decls` 把 L18/19/23/27/31/34 记成「未支持的顶层构造」6 条 —— 它们只是 `Char` 枚举的成员行，不是缺口（真缺口见类 G）。
4. **注释里的不是缺口**：L50 的 `asm x86_64 windows {` 在 `/* … */` 里；L534/L540 的 extern 调用被 `//` 注释。
5. **每个单元只报首个错误** → 类 D/E 的"计数"在 `--all` 里偏低（真实处数按全文件 grep 补上）。反之，一个单元里的多条同类错误只算一次 —— 所以"单元数"与"处数"两个数都给。
6. **49 份语料那个"模板插值"缺口不在前缀里**：`grep -c '`' ` = 0，全文件无反引号、无 `#if`/`in_scope`。
7. 并发：期间没遇到 exe 被截断/半写入（没有重试）。但另一个 worker 正在改 `simplexc/lower/**`，
   本文件里的 lowering 行号（`lower.cc:809/833/1666/1670/2154`、`prefix_globals()` L736）是 **2026-10-18 当时**抓的，
   改完要复核一次措辞与位置。

## 五、下一步（给主程的取舍材料，不是决定）

- 若只问"路线 A 差几类"：**前端 5 类**（A `$` / B `asm` / C extern 调用 / D `[T] expr` / E `<T> expr`）+ **声明层 3 类**（F 别名 / G 多行枚举 / H 39 个算子单元）+ **lowering 7 个内建**；
  其中 **H（算子+泛型+重载）** 与 **B（asm）** 是量级上的两块。
- 便宜的先行项（不牵动 lowering 架构）：G（多行枚举）、D、E、F —— 都是 parser/decls 几十行的活；
  A 的前端半边便宜、lowering 半边不便宜。

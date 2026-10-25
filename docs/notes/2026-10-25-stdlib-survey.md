# stdlib 前缀的构造普查（2026-10-25）

M5 要求「全量兼容 stdlib」：`simplexc/corpus/stdlib_prefix.si`（1503 行 / 27711 B，从
`compile_real.dll` 偏移 `0x119F48` 原样提取，拼在每份用户源码之前一起编译）必须能编。

结论已折叠进 `docs/facts/semantics.md` 的对应小节；本文记「怎么测出来的」与「哪些方向是死的」，
免得下一个人重走。

## 结构

1503 行 = 24 个 `const` + 7 个 `type`（`Char` 全 ASCII 枚举等）+ 79 个 `def` 与**裸头单元**。
其中 `$` 23 处 / `array(` 16 处 / `kernel32` 11 处 / ` asm` 7 处 / `@Type` 9 处 / `@Any` 9 处。

**它不是运行时库，是语言糖的定义**：大部分 `def` 是 `binary + (a: Ptr, b: AnyInt) Ptr { … }`
这类运算符重载 —— 补 stdlib 等于补一批语言构造，不是写一个库。

## 实测结论（每条都能用 `work/tmp/res-*/report.md` 里的探针复现）

| 构造 | 结论 |
| --- | --- |
| `$a` | **不是可变参数**。是「该形参体内会被改，调用点必须传左值」。参考原话：`1st will get modified, so put '$' in front when calling it.`；body 内一律写裸名。 |
| `binary`/`unary` | 能**覆盖内建**。同名多个版本时 **后声明者胜**，不是按具体度选（`a02`/`a02b`/`a02d` → `0xBBBB`/`0xAAAA`/`0xCCCC`）。 |
| `index_read`/`index_write` | **在参考里是死的**。5 种配置（Str、`Arr[U8]`、U32 下标、命名数组类型、写路径）全走内建，标记 store 从不出现；索引非数组基**直接崩**（`base_type.kind in {typ_array, typ_fixed_array}`）⇒ 不用实现。 |
| `asm` | 参考**重新汇编**（`mov rcx,32` → `mov rdx,rcx` + `mov ecx,0x20`）；助记符**白名单**（`nop` 被拒）；`asm aarch64` 整块**丢弃且不验证内容**（塞垃圾也编过）；活路径只需 7 条：`mov idiv shl or rdtsc rep movsb rep stosb`，无内存操作数/标签/`define`。 |
| `kernel32.'X'` | 产物形态是 `movabs reg,imm64 ; call reg`；**声明里的 DLL 名既不校验也不烘焙**（9 个名字里 4 个不是 kernel32 导出，`_write` 在 ucrtbase）；`extern`/`call_conv` 不调用时**产 0 字节**；`shadow_space` 改 `sub rsp,0x70`→`0x48`，`save_restore` 改 `movabs` 的目标寄存器；**5 个参数的调用被静默错译**（第 5 个进 `edx`，没有栈传参）。 |
| `@Type`/`@Any`/`@Size` | **类型参数**（26 处在类型位置 + 1 处 cast 头），不是内建函数。按调用点**单态化**；`size()` 折成**位宽**常量；`Arr[@Type]` 绑定**元素类型**；`@Any ≡ @Type`（两者产物字节完全相同）。**`return @Type random()` 本身就非法**（参考报 `Expected Type here.`，合法拼法是 `@<param-name>`）——前缀能编过只因 `random(@Type)` 从未被实例化，参考对泛型体是**惰性检查**。 |
| 嵌套 `def` | 参考完全接受（L1009 的 `def partition` 在 `quick_sort` 体内），按引用捕获，外部不可见。**我们已经支持**（`defs=1`、`@"outer$inner#L2"`、捕获作 `ptr`），只差一个回归用例。 |

## 我们的现状与缺口

`lexer.cc` **不用改**（`#`/`@`/`$` 都有 token，单引号是 string）；`parser.cc` 的 `$`（sigil 并进
`Param.name`）与索引 `a[i]`/`[T] expr` **已经通到 blob**。真正的缺口是三块：

1. **裸头单元声明**（`binary +` / `dot` / `index_read`）—— `decls.cc:match_def()` 只认 `def`；
2. **`call_conv` / `extern`** —— 已在 `9c59907` 认形状（留痕、不产代码）；
3. **`asm` 块** —— 编码器 `codegen/asm_x64.*` 已提交（7 助记符、16 条与参考逐字节对齐），缺的是接进流水线。

## 判据陷阱

`decls.cc` 的 `scan()` 把「未支持」的判定放在**本行花括号计数之后**，所以
`binary + (…) Ptr {` 这类**裸头开 `{`** 的行**永远不报错、也不进声明表**——它们被静默吞掉。
所以「错误行号变大」不等于真的支持了，必须同时看**声明数**。
标准库单独编译必然报「没有顶层语句块（程序根）」，那是前缀只有声明，不是卡点。

## 原始报告（`work/tmp/` 是临时目录，会清）

`work/tmp/res-sugar/`（`$` 与索引）、`res-type/`（泛型）、`res-asm/`（inline asm）、
`res-k32/`（kernel32）、`res-bare/`（裸头声明）。各含 `report.md` + 可复现的 `.si` 探针与参考产物。

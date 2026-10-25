# 设计：simplexc 接口冻结（主设计 §8 的 7 项）

- 日期：2026-10-16
- 状态：**冻结版 1**（负责人 2026-10-16 批准三处取舍：runtime 目标码随 blob 装配 / blob 内部禁止绝对重定位 /
  corpus 的 `.bin` 不进 git）。
  **版本 1.1（2026-10-17，增量）**：`AsmResult` 加 `stubs`（§4）+ 宿主 `stub` sidecar 协议（§6）——
  跨 boot 的 extern 解析让「产物里的 movabs 落在宿主镜像里」成为正常形态，原先的硬错误判据会把自己的产物挡住；
  同时 stub 不接桩 = 两遍跑没有确定性。改动理由与实现证据见 `docs/notes/2026-10-17-m1.5-probe.md`。
- **版本 1.3（2026-10-18，只有状态说明、不改任何接口）**：M3 期的 Python 原型
  （`frontend/*.py` + `lower/ir_build.py`）**已删除**（git 历史可查），产品实现 = 本文件冻结的 C++17。
  这意味着：**没有任何实现被当作语义权威**。语义以「原版编译器（游戏目录 `compile_real.dll`）的实测行为」为准，
  结论登记在 `docs/facts/semantics.md`；「跑不起来 / 结果不可复现」不再是改原型的理由（原型不在了）。
  版本 1.2（2026-10-17）曾把该原型列为只读参考实现，那条已随文件删除而失效。
- **版本 1.4（2026-10-18，改 §0/§1 的接口形状）**：编译器分四层（主设计 §3.2）。§1 从「前端产物 = 类型化 AST」
  拆成 **§1a 前端产物 = 无类型 AST** + **§1b sema 产物 = HIR 与类型表示**；
  `TypedProgram` / `parse_and_typecheck` 从未实现（实现是 `frontend::parse` + `lower/lower.cc` 内的 sema），
  按新分层改写。目录树：`lower/` → `sema/` + `codegen/`。
- 上游（唯一权威，本文件不复制其内容）：
  - 主设计 `docs/specs/2026-10-15-simplex-llvm-compiler-design.md`（产物契约 §2/§4、语义保真 §5、验证 §7、工作流 §8、里程碑 §9）
  - 主频层 `docs/specs/2026-10-15-freq-measurement-design.md`（变体轴、归属语义、`hook.log` 迁移）
- 证据等级：✅已证（跑过命令/读过字节）｜🔍推断｜❓未确认；**❓一律不得当已确认项用**（见 §9）
- 改动流程：改本文件的接口 = 改契约 → 必须同步通知所有在跑的写者，并在此文件登记版本号

---

## 0. 代码落库与所有权

```
reversing/simplexc/
  frontend/   词法/语法/ast（无类型）/#if in_scope 的裁剪点/span              → Ast
  sema/       名字解析/类型/consteval/单态化/内建识别/根确定与 DCE            → HIR
  codegen/    HIR → LLVM IR（内存模型、$ 写回、extern、entry 包装）           → LLVMModuleRef
  runtime/    路线 B 的运行时（C++17，编成 COFF，随 blob 装配）
  assemble/   COFF → blob（重定位、extern 桩、8 字节头、entry、生命周期）
  shell/      壳里**新增的那部分**（选实现、三模式、失败转交、核心入口）
  cli/        离线 CLI（--src/--out/--entry），与 shell 共用核心
  tools/      语料/判据/宿主/提取器等脚本
  corpus/     语料 + manifest（`ref/` 不进 git）
  dossier/    语义档案（机器可执行，§9 的实验都在这里落地）
```

- **语言分工（唯一权威）**：产品路径的 `frontend/` / `sema/` / `codegen/` / `runtime/` / `assemble/` / `shell/` / `cli/`
  = **C++17**（必要时汇编）；`tools/` 下的脚本 = Python（离线工具，不进产品路径）；
  **产品路径不得依赖 Python**（判据 = 主设计 §7.5 依赖门）。M3 期的 Python 原型已于 2026-10-18 删除。
- 既有 `reversing/tools/compile_hook.c` **继续演进，不做第二份壳**；`simplexc/shell/` 只放新代码。
- 一个写者一个目录（主设计 §8）；`tools/compile_hook.c` 由 `work/tmp/locks/compile-hook.lock` 串行化。

---

## 1. 层间产物：无类型 AST（前端）与 HIR（sema）

**语言与文件**：C++17；前端 `simplexc/frontend/ast.hh`、`simplexc/frontend/frontend.hh`；
sema 当前分散在 `lower/lower.hh` 的 `TyRef` 与 `lower.cc` 的函数里（函数体内不出现 `LLVM*` 的有 94 个、
1545 行；待搬进 `sema/`）。
（2026-10-18 更正：扩展名是 `.hh` 不是 `.h` —— 项目规则「C++ 用 `.hh`/`.cc`，C 用 `.h`/`.c`」，因为 pi-lens 按扩展名判语言，`.h` 会被当 C 检查，C++17 头里的 `std::`/模板会报假错。`ast.hh` 已落库并通过编译验证。）

### 1a 前端产物：无类型 AST

**只负责形状**：节点种类、子节点、源码 span。**不做**类型推导、不解析泛型实参、不判断可达性。

```cpp
// 冻结：无类型 AST（当前实现形态见 ast.hh）与预处理裁剪
struct Node { NodeKind kind; Span span; /* 子节点依 kind 而异 */ };
struct CompileOptions { bool allow_unsupported = false; /* 恒 false：见 §8 */ };
Result<Ast, ParseError> parse(std::string_view src, const CompileOptions&);
```

**编译期构造**（U12 实测：98 份语料里 `#` 只出现两类）：(a) `#if <const-expr>{ … }`（语料里唯一形态是
`#if in_scope("name"){ … }`）；(b) `const #NAME = <const-expr>`。`in_scope("name")` = "该名字在此处可见（含后置定义）"
→ 必须做**两遍/不动点**判定；判定结果是**保留/删除子树**，不产生类型信息。

### 1b sema 产物：HIR 与类型表示

**当前形态**：类型标识是 `lower/lower.hh` 的 `TyRef`；下面的 `Type`/`TypeKind` 是**目标形态**（从未实现过，
2026-10-18 核实）。HIR 必须满足主设计 §3.2 的六条不变量。

```cpp
// 冻结：类型标识 ------------------------------------------------------------
enum class TypeKind { Error, Void, Bool, Int, Uint, SizedInt, Ptr, Array, Struct, Enum, TypeValue };
struct Type {
  TypeKind kind;
  int  bits;            // SizedInt：位宽 1..64（U1/U5/U8/U16/U32/U64 都合法，不是 2 的幂也合法）
  bool is_signed;       // SizedInt
  const Type* elem;     // Array / Ptr 的元素类型（Ptr 的 elem = void）
  struct StructInfo* stru;
  struct EnumInfo*   enu;
};

// 冻结：类型等价 ------------------------------------------------------------
// Bool ≡ SizedInt{1,unsigned}（Bool 是别名）；Int ≡ SizedInt{64,signed}；Ptr 与 U64 是不同类型，
// 互转必须显式 raw_cast 或走指针算术规则。
// Uint / Sint 不是具体类型，是**重载族约束**（匹配任一无符号/有符号整数），只允许出现在参数类型位置
// （依据：stdlib 前缀 `def str(number: Uint) Str`）。

// 冻结：泛型 = 单态化 -------------------------------------------------------
// @Type → TypeValue 参数；@Size → 编译期整数参数；@Any → 任意类型参数。
// 实例化名 = "<函数名>$<实参规范化字符串>"（同一 (函数, 实参, 环境类型) 只实例化一次；不含调用点）。
// 实例化集合在编译期封闭；不留运行期泛型分发。

// 冻结：程序根 -------------------------------------------------------------
// 程序根 = 顶层语句块（含全局 var 初始化），末尾隐式 thread_exit()（依据：主设计 §2.4 ✅）。
// entry 每次编译只进入 1 次 → 全局数据的唯一初始化点就在顶层块，不需要运行期守卫。

// 冻结：编译期构造 ---------------------------------------------------------
// 只有两类（U12 实测：98 份语料里 `#` 只出现这两类）：
//   (a) `#if <const-expr>{ … }`  —— 语料里唯一形态是 `#if in_scope("name"){ … }`
//   (b) `const #NAME = <const-expr>`
// in_scope("name") = "该名字在此处可见（含后置定义）" → 必须做**两遍/不动点**判定。

// 冻结：sema 接口（目标形态；当前实现 = frontend::parse 之后，lower 内的 sema 直接消费 AST）
struct CompileError { Responsibility resp; Stage stage; Span span; std::string detail; };  // 三轴见主设计 §3.4
Result<Hir, CompileError> analyze(const Ast&, SemaContext&);
```

- `Hir` 必须给 codegen 提供：`main` 块、函数表（含单态化实例）、全局 var 表、编译期常量表、
  `extern` 声明表、内建调用点表、每个表达式/左值的**解析后类型**、每个节点的 span。
- **未支持构造一律 `CompileError`**（§8）。禁止"猜一个语义"。
- 布局/宽度规则（struct 布局、enum 底层宽度、整数提升与截断）**不在本文件冻结**，由 §9 的实验给出后写进
  `docs/facts.md`，前端只允许调用唯一实现 `layout_struct()` / `enum_storage_bits()`。

---

## 2. 内建表 schema（唯一来源）

- 单一来源文件：`simplexc/codegen/builtins.def`（X-macro，一处定义，前后端与 runtime 都从它生成/校验）。

```c
// BUILTIN(名字, 签名, 是否 mutating, 副作用, 对齐, lowering)
BUILTIN(load,   "load(<T>, Ptr) -> T",        no,  read,  natural, LLVM_LOAD)
BUILTIN(store,  "store(Ptr, T) -> Void",      YES, write, natural, LLVM_STORE)
BUILTIN(raw_cast,"raw_cast(<T>, V) -> T",     no,  none,  - ,      BITCAST)
BUILTIN(grow,   "grow(arena?, Arr[T], Int) -> Arr[T]", YES, alloc+copy, - , RT_grow)
...
```

- 每行必须写清：**参数（含类型参数）、返回类型、是否 mutating、副作用、对齐要求、lowering 目标**。
  mutating 判定错 = 语义分叉（`swap` / `partition` / `grow` / `shrink` / `join` / `append` / `store`）。
- lowering 目标三种：`LLVM_*`（直映射）、`RT_*`（调用我们 runtime）、`INLINE`（在 lowering 期内联展开）。
- **权威清单（三路交叉，缺一路不算钉子）**：
  1. `compile_real.dll` 的字面量池 —— `mac_*` 名表已实测提取到（17 个：`mac_in_scope, mac_uint_of_size,
     mac_raw_cast, mac_type_of_element, mac_type_of, mac_append, mac_shrink, mac_grow, mac_join, mac_clone,
     mac_array, mac_empty_array_address, mac_default, mac_int_of_size, mac_store, mac_load, mac_size`），
     提取器 `reversing/tools/extract_nim_strings.py`；**裸名表（store/min/max/clz/ctz/asr/lsr/lsl/ror/rol/
     bswap/…）也在同一池里，但类别分组（STANDARD/UNARY/BINARY/BINARY_MUTATING）尚未从容器结构确认 → §9 U-L13**。
  2. stdlib 前缀里「被调用但未定义」的名字。
  3. 98 份语料里「被调用但未定义」的名字。
- 附录 A = 我这次实测的 (2)+(3) 并集，作为 M3 的起点清单；**它还不是冻结的完备集**。

---

## 3. runtime ABI（路线 B）

**实现语言/形态**：C++17，`w64devkit g++ -c` 编成 COFF，由装配器**与 LLVM 产出的目标文件一起链接进 blob**（自包含）。
不做常驻 runtime DLL（已批准项 a）。

**调用约定（冻结）**
- Win64 ABI：整型/指针参数 `rcx, rdx, r8, r9`，其余走栈；调用方留 32 字节影子空间；`call` 时 `rsp ≡ 0 (mod 16)`。
- callee-saved：`rbx, rbp, rdi, rsi, r12–r15`；xmm6–xmm15 在 Win64 上也是 non-volatile。
- runtime 函数**不得递归**、不得依赖大栈（线程栈大小未知，主设计 §5 第 8 条）。

**数组/字符串布局（冻结：必须与 Nim ARC/ORC seq 同构）**

```
payload p 指向第 0 个元素
p[-8] : u64 = (len << 16) | cap_log2     // len 是 48 位，cap_log2 是低 16 位
p[-2] : u16 = cap_log2                    // get_capacity = (1 << cap_log2) >> 1 字节
len   = u64(p-8) >> 16
```
依据（✅ stdlib 前缀实测，`stdlib_prefix.si` 的 `allocate_raw`/`get_capacity`/`get_length`/`set_length`）：
`store(arr_ptr, (length << 16) | arr_cap)`、`get_length = load(<Int>, p-8) >> 16`、`get_capacity = to_capacity(u16(p-2))`、
`to_capacity(l) = (1<<l)>>1`。
**为什么不是自由选择**：游戏把自己的 seq/字符串按硬编码绝对地址交给生成代码
（`var input_replay = raw_cast(<Arr[U64]>, 2416573480960)`、`const #SIMULATION_STATE = Ptr 2416623026176`，
`work/hook/all/hook_src_001.si:5,11`）→ 长度/容量字段必须同构，否则读游戏数组就分叉。

**arena（冻结）**：我们自己的 arena，实现自由；**不依赖游戏给的 `#DATA_*` 地址**（那些是游戏数据段里的 RAM 数据，
不是我们的分配区）。arena 元数据放 blob 的 `.data/.bss`（blob 每次编译被整体 memcpy → 每次进入都是干净状态）。
空数组哨兵 `empty_array_address()` 的**具体值待实测**（§9 U-L9），冻结其语义 = 「长度 0、可安全读写 0 字节」。

**字符串**：`Str = Arr[Char]`，`Char` = 1 字节无符号（0..255）。是否 NUL 终止、字面量编码见 §9 U-L3/U-L10。

**谁保存寄存器（冻结）**：入口 shim 保存全部易失与非易失通用寄存器、把栈对齐到 16 字节后再 `call` 我们的
`main`；LLVM 生成的函数一律按 Win64 callee-saved 规则行事。

---

## 4. 装配器接口

```cpp
struct ObjectFile  { std::string name; std::vector<uint8_t> bytes; };   // COFF
class ExternResolver {                 // 外部符号 → 绝对地址；由壳实现（GetProcAddress 之类）
  virtual bool resolve(std::string_view symbol, uint64_t& addr) = 0;
};
struct Blob {
  std::vector<uint8_t> bytes;   // bytes[0:8] = out_len | (1<<62)；机器码从 +8 起
  uint64_t entry_off;           // 相对机器码起点（= 相对 blob+8）
};
enum class AsmStatus { Ok, BadObject, UndefinedSymbol, UnsupportedReloc, TooLarge };
struct StubRef    { uint64_t offset; std::string symbol; };   // offset 相对机器码起点；顺序 = 造桩顺序
struct AsmResult  { AsmStatus status; Blob blob; std::string detail; std::vector<StubRef> stubs; };

AsmResult assemble(const std::vector<ObjectFile>& objs,
                   std::string_view entry_symbol,       // = 我们的 shim 符号 __tc_entry
                   ExternResolver& resolver,
                   const AsmOptions& opt = {});
```

（与 `simplexc/assemble/assemble.hh` 一致。`InternalRelocToExtern` 已并入 `UnsupportedReloc` —— 内部符号的绝对重定位
就是被拒的那一类；`BadObject` = COFF 解析不了；`TooLarge` = `out_len > AsmOptions::max_blob`。）

**冻结的装配规则**
1. 输入是 COFF 目标文件（`LLVMTargetMachineEmitToMemoryBuffer(..., LLVMObjectFile)` 的字节）；
   解析节/符号/重定位，**复用 `reversing/tools/coff_symbols.py` 已探明的解析方式，不另起一套**。
2. 允许的重定位：`IMAGE_REL_AMD64_REL32`(0x04) 及其 `+1..+5` 变体（内部符号）。
   **任何指向内部符号的绝对重定位（`ADDR64`=0x01 / `ADDR32`=0x02 / `ADDR32NB`）一律 `UnsupportedReloc` 硬错误**
   （已批准项 b：blob 会被拷到任意地址执行，内部绝对地址必然失效）。
3. 未定义符号：按「stub pool + 编译期解析」处理 —— 每个未定义符号在 `.text` 尾部分配一个
   `movabs rax, imm64 ; jmp rax`（10 + 2 = **12 字节**）桩；call/jmp 用 `REL32` 指向该桩（同一 blob 内，必在 ±2GB）。
   `GetProcAddress` 失败的符号 → `UndefinedSymbol`（不是崩溃）。
4. 整节丢弃 `.pdata` / `.xdata` / `.debug*`；IR 层禁 `uwtable`、禁异常。
5. `__chkstk` / `___chkstk_ms` 由 `runtime/` 提供（我们自己的页探测实现），不依赖 CRT。
6. 产物上限：`out_len ≤ 64 MB`（与转发层 `dump()` 的上限一致）；超出 → `TooLarge`。
7. 头 8 字节 = `out_len | (1<<62)`（bit62 置位 → 游戏不 free，主设计 §2.3/§4）；**头里不放别的元数据**。
8. 缓冲分配：壳里 `malloc(out_len + 8)`（多 8 字节 padding，主设计 §4 第 1 条），生命周期由壳持有：
   **下次编译开始时释放上一块**。
9. entry 符号 = 手写 shim `__tc_entry`：保存寄存器 + 对齐栈 + `call <llvm_main>` + 收尾（`thread_exit`），
   因为游戏是 `jmp` 进来的、不保证 ABI 对齐（主设计 §5 第 8 条）。

**输入形状已实测（2026-10-16，`simplexc/shell/llvm_link_probe.cc`）**：
`x86_64-pc-windows-gnu` + **RelocMode=PIC** + CodeModel=Small + `default<O2>` 下，
- 节：`.text`/`.data`/`.bss`（空的也在），有栈帧的函数额外带 `.pdata`/`.xdata`；
- `.text` 重定位**只有** `IMAGE_REL_AMD64_REL32`，出现三种目标：内部函数、未定义 extern、**`___chkstk_ms`**
  （>4 KiB 栈帧才出现）；
- `.pdata` 用的是 `IMAGE_REL_AMD64_ADDR32NB`（指向 `.text`/`.xdata`）→ 正好被第 4 条「整节丢弃」干掉；
- 未定义符号在 COFF 里是 `sec 0` + `scl 2`。

---

## 5. DLL 壳（`tools/compile_hook.c` 演进 + `simplexc/shell/`）

**模式（冻结）**：`opt.ini` 的 `[opt] compiler = ref | ours | ours_with_fallback`；**缺失键兜底 `ref`**
（fail-safe：绝不能把"没配"当成"我们的实现"）。

| 模式 | 我们编译成功 | 我们编译失败（未支持/内部错误/超时/熔断） |
|---|---|---|
| `ref` | —（不调用我们的编译器） | — |
| `ours_with_fallback` | 用我们的产物 | **整单转交 `compile_real.dll`，原样返回它的 Result** |
| `ours` | 用我们的产物 | **不转交**：返回自造失败 Result（`out[3]≠0`），游戏走失败分支，不跑新代码 |

- 自造失败 Result（仅 `ours` 模式需要）：
  `out[3] = len`、`out[4] = 指向 { u64(len | (1<<62)), char data[len], NUL }` —— bit62 置位 → 游戏不 free
  （依据：主设计 §2.3 的门控 + §2.2 的失败判定）。内容前缀固定 `ours: ` + 错误分类，供回退率统计。
- **熔断**：VEH（`AddVectoredExceptionHandler`）包住整次调用；触发后本进程内永久切 `ref`
  （记 `error stage=ours_fault`）。耗时超预算（默认 5 s，调试期 60 s）→ 记 `error stage=ours_timeout` → 按模式回退。
  失败一律**不崩游戏**：任何异常路径都返回一个形状合法的 Result。
- 记录（冻结）：
  - `impl` 词表（**唯一一处定义**，manifest 列与 hook.log 行共用）：`ref`（配置为参考）/ `ours`（用了我们的产物）/
    `fallback`（`ours_with_fallback` 且 ours 失败 → 参考产物）/ `ours_failed`（`ours` 模式失败，未转交）；
    （词表变更：`reference` → `ref`，2026-10-25，负责人批准 —— 配置值 `compiler=` 与日志词表一起改，不存在别名）
  - `manifest.tsv` **末尾追加 `impl` 列**（既有列序号不变），**每次调用都写**；
  - `hook.log` 新增 per-call `impl` 行（键：`impl` / `mode` / `result`）—— **含 ref 模式**：
    回退率需要分母，只在「非 ref」时记会把「绝大多数调用都是 ref」这个事实藏起来；
  - 归属只认 `hook.log` 的 `opt` / `impl` 行（主设计 §7.3）。
- 三模式与 `rewrite` 正交（`rewrite` 仍只由改写器定义合法值集合）。

**导出与 C→C++ 边界（冻结，2026-10-17）**：

- DLL 必须靠**名字**匹配：`compile` / `NimMain` / `NimDestroyGlobals`
  （游戏是 `nimLoadLibrary("compile.dll")` + `nimGetProcAddr("compile")`）。现状是纯 C，用 `__declspec(dllexport)`
  （`tools/compile_hook.c:763,869,876`）；**C++ 化后必须补 `extern "C"`**（否则导出名变成 `_Z7compile…`，
  游戏查到 NULL）。**不必**用 `.def` 导出别名去绕名字改编（GNU ld 的 `EXPORTS alias=internal` 可用，
  代价是内部名随编译器/ABI 变而脆）。
- 导出签名**只允许 C 类型**（调用方是 Nim，不知道我们的 C++ ABI）：C++ 类型不得出现在参数/返回值里。
- **异常不得穿出边界**：C++ 异常必须在壳内 catch-all（穿过 Nim 帧的展开没有保证：轻则漏跑析构，重则直接崩），
  catch-all 是**唯一出口**，转成形状合法的失败 Result（`out[3]≠0`）。**不要**用 `-fno-exceptions` 回避 ——
  那会把 `bad_alloc` 变成 `terminate`/`abort`，就是崩游戏。
- 加载期（`DllMain`）的**硬禁忌**是 `LoadLibrary` / `CreateThread` / 等待其他线程（loader lock 下真会死锁）；
  内存分配尽量推迟（分配本身通常可用），别在加载期干重活。

---

## 6. 差分宿主驱动协议

**形态（冻结）**：宿主是**独立进程**（`simplexc/tools/host/`），输入 = blob 文件 + 地址表 + 命令序列，
输出 = 逐 tick 快照 JSONL；不依赖游戏、不需要 `game.lock`。

- **假游戏内存**：生成源码把所有游戏地址写成字面量（`Ptr <digits>`、`raw_cast(<T>, <digits>)`、`#DATA_*`），
  产物里对应 `movabs imm64` → 宿主按 64 KB 页 `VirtualAlloc` 预留这些地址区间。
  **产物里出现「地址表外 + 本进程 `VirtualQuery` 显示已映射」的 movabs 立即数 = 宿主报错退出**
  （宁可吵，也别静默错）。判据为什么不是「一切表外 movabs」：实测每份参考产物都有 4 处大整数常量
  （`0x2540be400`、`0x19db1ded53e8000` 之类）；真正会静默错的是**能解引用**的那些，纯常量只统计。
- **extern 中和**（⚠️ 2026-10-17 按实测更正；原文「把指向 kernel32 的 movabs 改写成宿主桩」是地址区间的误读）：
  参考产物的 movabs 全部落在 **`compile_real.dll` 自己的镜像**里 —— 导入 thunk / DLL 内部代码 / DLL 的 `.bss` 全局，
  **不是 kernel32 自身的地址**。所以宿主必须 **`LoadLibrary` 真 `compile_real.dll`** → 把产物里 baked 基址
  重定位到实际加载基址（加 delta）→ 把 DLL 的 **IAT 槽**换成宿主桩；桩记录 `{name, args, ret}` 进快照。
  取证见 `docs/facts/corpus-host.md` §「参考产物会直接调用 compile_real.dll 里的运行时函数」+ `docs/notes/2026-10-17-m2-host-design.md`。
- **`stub` sidecar（版本 1.1 增量）**：**我们自己的**产物形态不同 —— 装配器把未定义符号解析成 `.text` 尾部的
  `movabs rax, imm64 ; jmp rax` 代码桩（以及 `__imp_*` 的 8 字节数据槽），`imm64` 是**编译期**在别的进程里
  `GetProcAddress` 得的绝对地址（`Sleep` 等）→ 它会落在宿主进程里「已映射但不在地址表内」的位置，
  正好撞上下面那条硬错误判据，**而且不接桩就没有两遍跑的确定性**。所以：装配/CLI 在产物旁写
  `<blob>.stubs`（TSV：`offset<TAB>kind<TAB>symbol`，`kind ∈ {stub, slot}`，`offset` 相对机器码起点），
  宿主读了它就把这些位置按符号接到自己的桩上（记录格式与 IAT 槽路径**完全一致**，否则 extern 序列不可比）；
  命中不了桩的符号 → 报名字并非 0 退出。**游戏内不需要 sidecar** —— 壳自己用 `GetProcAddress` 填桩。
- **命令驱动**（依据主设计 §7.1）：命令块在游戏内存里（`commands[...]`/`settings[...]`），
  驱动方把 `sim_target_cycle` 依次设为 `1,2,3,…` 并递增 `ctl_command_id` 发 `SimCommand.run`
  → 每次 `run` 返回即一个 tick 边界。
- **快照**：`{tick, pages: {addr → sha256(page)}, extern: [...], alive}`；差分 = 逐 tick 比对
  `pages` 与 `extern` 序列（逐字节，不做近似）。
- **白名单**：arena/分配器内部布局等不可观测差异必须显式列白、可审计、尽量小（主设计 §7.1）。
- **自比对基线**：参考产物 vs 参考产物 必须逐 tick 全等（M2 完成判据）。
- 超时：每 tick 有预算，超时记 `timeout` 并杀宿主进程；宿主崩溃也要留下「最后成功的 tick」。

---

## 7. 构建与链接

- C/C++：`D:\bin\w64devkit\bin\g++.exe`（GCC 14.2），`-std=c++17 -O2`，**直调 gcc，不引入 CMake**
  （照抄 `tools/deploy_compile_hook.py` 的构建风格）；构建脚本 `simplexc/tools/build.py`。
- LLVM：**`LLVM-C.dll` + C API**，版本锁定；三元组 `x86_64-pc-windows-gnu`，**`RelocMode=PIC`**，`CodeModel=Small`，
  禁 `uwtable`/异常。管线：`LLVMRunPasses`（`-O2`）+ `LLVMTargetMachineEmitToMemoryBuffer(..., LLVMObjectFile)`。
- 取导入库：**已定走 ②**（① 未采用）—— ① MSYS2 `mingw-w64-*` LLVM 包（MinGW 构建，与目标三元组同源）；
  ② 官方 win64 发行包 + `gendef LLVM-C.dll` → `dlltool -m i386:x86-64 -d LLVM-C.def -l lib/libLLVM-C.a`
  （官方包只有 DLL，无 C++ 库/头）。取件与验证命令见 `docs/facts.md`。
- 落地位置：DLL 与导入库放 `reversing/build/llvm/`（**不进 git**）；版本号 + DLL sha256 记进 `docs/facts.md`。
- 导出机制：见 §5（`extern "C"` + `__declspec(dllexport)`），只导出那三个符号。
- 部署：`build/compile_hook.dll` 拷进游戏目录时**改名 `compile.dll`**；一旦链接 `LLVM-C.dll`，
  部署必须把 `LLVM-C.dll` 一起拷进去（否则游戏加载时解析依赖失败）。
- 构建产物：**现状** = `reversing/build/compile_hook.dll`（壳）、`build/tcblob.exe` / `build/llvm_prog.exe`（离线 CLI）；
  `simplexc.dll` / `simplexc-cli.exe` 是**接上前端（M4.5）之后**的目标名，现在还不存在
  （见 `simplexc/cli/README.md` 的“现状”节）。
- 不依赖 PATH：工具链一律**绝对路径**（`W64DEVKIT_BIN` / `W64DEVKIT_CXX` / `W64DEVKIT_CC` 可覆盖，见 `build.py`），
  入口只有 `build.py`。make/CMake 只在出现“全量重建太慢”或“改 header 未重编”时再引入（入口不变）。

---

### §7 附注：为什么是 PIC 而不是 Static（2026-10-17 更正）

产物会被游戏整体 `memcpy` 到**编译期未知**的 JIT 内存再执行（`MapViewOfFileEx` 基址每次不同），
所以位置无关是唯一正确的模型。实测：`RelocMode=Static` 下 LLVM 对「往内部全局写常量」这类访问会生成
`movq $imm32,[disp32]` 绝对寻址，产物里出现 6 处 `IMAGE_REL_AMD64_ADDR32`（指向我方内部全局
`cycle`/`last_command_id`），与 §4 第 2 条「内部符号的绝对重定位一律 `UnsupportedReloc`」直接冲突；
改 PIC 后 `.text` 只剩 23 处 `REL32`（+ `.pdata` 的 `ADDR32NB`，按 §4 第 4 条整节丢弃），装配通过。
旁证：clang 对该三元组本来就默认 `-mrelocation-model pic` —— `probe/006_equiv.c` 那条已验过的链路走的正是 PIC。
**§4 第 2 条保留不变**：PIC 让它自然成立。

## 8. 错误模型与回退契约（横切）

- 错误分类（回退率报告按此分类）：`UnsupportedConstruct` / `InternalError` / `Timeout` / `OursFault` /
  `BlobTooLarge` / `RelocUnsupported` / `SymbolUndefined`。
- **禁止静默降级**：任何不支持 → 显式错误 + （按模式）回退；不允许"猜一个语义"或"悄悄转交原版编译器"。
- 每条错误必须能定位到源码 span（`UnsupportedConstruct` 尤其），供覆盖率矩阵与最小复现。
- `CompileError` 与 §5 的记录字段一一对应，别在别处再定义一套分类；三轴（责任/阶段/位置）是归类维度，
  本节的 7 类是责任与阶段的细化标签，二者不得互相取代（主设计 §3.4）。

---

## 9. 未冻结 / 必须先实验的项（❓，禁止当已确认用）

这些全部由 **M3 语义档案**（`simplexc/dossier/`，主设计 §7.4）产出，每条 = 最小 `.si` + 驱动方式 +
参考产物下的可观测状态变化 + 结论（写进 `docs/facts.md` 或 `docs/pitfalls.md`）：

| # | 问题 | 影响 |
|---|---|---|
| U-L1 | ~~`load`/`store` 的地址单位与对齐要求；`load(<U5>, p)` 这类非字节宽度怎么读~~ **已闭合（2026-10-18）**：字节地址、无对齐要求；`N≤32` → `ceil(N/8)` 字节，`N>32` → 8 字节零扩展写（会清邻居）、`N<8` 为整字节访问；验证见主设计 §5.1 / `docs/facts/semantics.md` | 内存模型的地基 |
| U-L2 | `arr[i]` 越界语义（trap / 未定义 / 掩码） | 索引 lowering |
| U-L3 | `Str` 是否 NUL 终止；`+` 拼接的分配语义 | 字符串 runtime |
| U-L4 | struct 布局与对齐规则（字段序、padding、嵌套） | 前端布局 + `raw_cast` |
| U-L5 | enum 底层宽度规则（成员数 → 1/2/4/8 字节） | 前端类型系统 |
| U-L6 | `$` 参数写回时机（每次赋值 or 调用返回） | lowering 的别名/写回 |
| U-L7 | 整数提升 / wrapping / 隐式截断规则（`U8 x = ...` 与 `U8(v)` 的区别） | 语义保真的核心 |
| U-L8 | 除零 / 移位越界 / 有符号右移的行为 | 未定义行为映射 |
| U-L9 | `empty_array_address()` 的哨兵值 | 空数组读写 |
| U-L10 | 字符串字面量编码（`\n` 转义、是否带长度头） | runtime 字符串 |
| U-L11 | `type_of(x)` / `size(x)` 的精确语义（对值 vs 对类型；返回类型还是常量） | 内建表 |
| U-L12 | `min` / `max` 的类型规则（同型？提升？） | 内建表 |
| U-L13 | `INTRINSIC_*_NAMES` 四张表的**分组与完备内容**（从 `compile_real.dll` 的容器结构提取） | 内建表完备性（U5 的钉子） |
| U-L14 | `#if in_scope` 的名字可见性规则（含前向引用、`@Size` 实例化影响） | 预处理 |

---

## 附录 A：内建/未定义名清单（实测并集，M3 起点）

来源 ①stdlib 前缀（`stdlib_prefix.si`，1503 行）里「被调用但未定义」：
`clz, empty_array_address, grow, index_read, index_write, join, load, raw_cast, shrink, store, type_of_element`
（另有一批是**方法糖**：`.len/.push/.pop/.contains/.next/.min/.size/.write/.array/.cstring` 与时间单位 `.ms/.ns/.us/.sec/.day/.hour`）。

来源 ②98 份生成源码里「被调用且本文件未定义」（按出现文件数）：
`store 98, raw_cast 98, load 97, min 97, type_of 27, asr 23, size 23, array 16, hex 13, ror 2, clz 2, str 3, bin 1`
（`in_scope` 是编译期谓词；`sleep/get_time/memory_clear/memory_copy/random` 等由前缀定义，不是内建）

来源 ③`compile_real.dll` 字面量池的 `mac_*` 名表（17 个，见 §2）。

**注**：`RAW`/`INTRINSIC` 的完整分组与大小写规范见 U-L13；本表只用于 M3 打底，不能当规范。

---

## 证据索引

| 事实 | 复现方式 |
|---|---|
| 数组布局 = `(len<<16)\|cap`，`get_capacity = (1<<cap)>>1` | 读 `stdlib_prefix.si` 的 `allocate_raw`/`get_length`/`set_length`/`get_capacity` |
| 游戏把自己的数组硬编码交给生成代码 | `grep -n 'raw_cast(<Arr\|#SIMULATION_STATE' work/hook/all/hook_src_001.si` |
| `#` 只有 `#if in_scope` 与 `const #NAME` 两类 | `grep -oh '#[A-Za-z_][A-Za-z0-9_]*' work/hook/all/*.si \| sort -u` |
| 内建/宏名表在 DLL 的字面量池里 | `reversing/tools/extract_nim_strings.py <dll>` → `mac_*` 组 |
| extern 调用 = Win64 ABI + `sub rsp,imm16` 影子空间 | 反汇编任一 `work/hook/all/hook_out_*.bin`（U10）；调用目标是 `compile_real.dll` 的导入 thunk，见 `docs/facts.md` |

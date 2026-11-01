# 设计：Simplex → LLVM 编译器（drop-in 替换 compile.dll）

- 日期：2026-10-15
- 分支：`main`（本设计尚未落代码）
- 状态：**设计定稿；实现进行中**（M4.5 已完成，M5 卡点见 `docs/status.md`）。产物契约由 T1–T5 实测闭合（见 §2、§4）。
  2026-10-15 负责人确认：§7.5 判据入口 / §8 agent 工作流 / §10 版本锁定与重验脚本；代码落库位置 = `simplexc/`；
  **LLVM 链接方式 = `LLVM-C.dll` + C API + 现有 w64devkit g++**（不引入 CMake；构建方式照抄
  `tools/deploy_compile_hook.py` 的直调 gcc）；语料实测见 §2.7；MVP 验收下界 = 沙盒模式能跑起来。
- **2026-10-18 重构**：确定四层分层（`frontend`→无类型 AST → `sema`→HIR → `codegen`→LLVM IR → `assemble`→blob），
  据此改 §3.1、新增 §3.2–§3.4 / §5.1–§5.2 / §7.6 / §8 回退段；`lower/` 改名 `codegen/`、新增 `sema/`。
  动因：`docs/notes/2026-10-18-frontend-audit.md`。
- 相关文档：`docs/facts.md`（复用结论）、`docs/status.md`（当前进度）、`docs/pitfalls.md`（踩过的坑）
- 证据等级：✅已证（跑过命令 / 反汇编核对）｜🔍推断（由实测数据推得）｜❓未确认

---

## 1. 目标与非目标

**目标**：把游戏目录的 `compile.dll` 换成自研实现 —— 用自研前端把 Simplex 源码编译成 LLVM IR，交给 LLVM
优化，再装配成游戏可直接 JIT 执行的机器码块，提升游戏内电路仿真主频。

**正确性判据**：同一份 `.si`，我们的产物与参考产物**逐 tick 状态等价**（定义见 §7.1）。

**主频测量**：`tools/watch_freq.py`，benchmark 用 RV32IM 电路（很远，暂不做）。

**非目标**：
- **不做转译**（Simplex → C/C++ 再交给别的编译器），理由见 §5.3。
- 不修改游戏本体（`Turing Complete.exe` / `game_engine.dll`）；不重写游戏侧代码生成器。
- 不做关卡编辑器 / UI 相关工作。
- **平台非目标**：只做 Windows x86-64。原版编译器另有 aarch64 / macOS 后端，我们不做。

**交付形态**：drop-in DLL。游戏按文件名 `compile.dll` + 符号名 `compile` 加载
（✅ `nimLoadLibrary("compile.dll")` @`0x14021d40b` + `nimGetProcAddr("compile")` @`0x14021d426`）。
原版编译器保留为 `compile_real.dll`（现网转发层已如此部署）。

---

## 2. 已实测的系统事实

### 2.1 调用契约（✅）

```c
Result* compile(Result* out /*rcx*/, NimStr* src /*rdx*/, int flags /*r8d*/);
// NimStr = { int64 len; void* ptr; }   字符数据在 ptr + 8
// Result = 40 字节，游戏侧落在栈上 rsp+0x20..0x48
```
- 导出表只有 3 个符号：`compile` / `NimMain` / `NimDestroyGlobals`。
- **`flags` 恒为 0**：调用点 `0x14021cff6 xor %r8d,%r8d` → `0x14021d004 call *compile`；
  `work/hook/manifest.tsv` 9 次调用全为 `0x0`（✅ 两路独立印证；那 9 行来自一次会话、同一份源码）。
- 全 exe 只有**一个**调用点（`0x14021d004`，在 `handle_request_compile_and_run` 内，跑在 compile 线程上）。
- 可脱离游戏进程调用；同一源码在同一 rewrite 模式下产物逐字节确定
  （✅ `manifest.tsv` 002–006 五次同源同 sha；跨进程亦可复现，见 `facts.md`）。

### 2.2 Result 的字段语义（✅）

| 字段 | 含义 | 成功值 | 失败值 |
|---|---|---|---|
| `out[0]` | 产物缓冲总长（含 8 字节头） | 码长 + 8 | 0 |
| `out[1]` | 产物缓冲指针 | 有效指针 | 0 |
| `out[2]` | `entry_off`（相对机器码起点） | 顶层块偏移 | 0 |

- `out[2]` 游戏按 **32 位**取（`0x14021d067 mov 0xc0(%rsp),%edi` → 线程上下文 `mov (%rbx),%edx`），
  但末跳按 **64 位**用（`lea (%rsi,%rdi,1),%rax`）。我们的槽必须**8 字节写全、高位清零**。
| `out[3]` | **错误消息长度**（非 0 即失败） | 0 | 非 0 |
| `out[4]` | **错误消息指针**（Nim 字符串形状） | 0 | 有效指针 |

- 失败判定：`0x14021d07b mov 0x80(%rsp),%rbx`（= `out[3]`）→ `0x14021d083 test %rbx,%rbx`
  → `jne 0x14021d198`（失败路径，**跳过** `send_command(1)` 与 `createThread`，不跑新代码）。
- 成功路径：`0x14021d08c mov $1,%ecx; call send_command` → `0x14021d0f5 createThread(jit_function)`。
- ⚠️ **失败时只给 `out[0]=0` 不够**：若 `out[0]=0` 且 `out[3]=0`，游戏走成功路径起 jit 线程，
  `len<=0` 触发 `raiseIndexError2`（`jit` @`0x14021ccd9`）。失败**必须**给非 0 的 `out[3]`。
- 返回值（16 字节 Nim string，sret）形状：成功 `{0, nil}`（`0x14021d13a xor ebx,ebx`）；
  失败 `{out[3], out[4]}`（`0x14021d17a/0x14021d17e`）。
- `out[3]`/`out[4]` 是一对 Nim 字符串 `{len, ptr}`，游戏对它**只 move、无 refcount**；
  最终释放在主线程消费处，同样是 bit62 门控 + `deallocShared`（✅ 引用流已追；UI 显示点 ❓）。

### 2.3 产物形态与内存所有权（✅）

- 产物 = `[8 字节头][机器码]`，`out[1]` 指向头，机器码从 `out[1]+8` 起。
- **8 字节头的值游戏从不读**（✅ T4）：全 exe 只有 `jit` 的 `memcpy(ptr+8, len)` 一处读产物。
- 但头的**一个 bit 会被读**：`eqdestroy___pureZtimes_u2668` @`0x14001c260` 做
  `testb $0x40,0x7(p)` —— 看头 word 的 **bit 62**；置位 → `ret`（不 free），否则
  `rawDealloc(allocator__system_u7164, p)`（🔍 该 bit 即 Nim 的 `NIM_STRLIT_FLAG`；旁证：
  exe 内静态字面量 `0x14053c2e0 = 34 00 00 00 00 00 00 40` = 52 | bit62）。
- `out[4]`（错误消息）走同一门控：成功路径 `0x14021d11a`→`0x14021d127`→`0x14021d300 deallocShared`；
  失败路径 `0x14021d203`（此处读到的是已被 `eqwasMoved` 清零的副本，恒为 no-op）→ `0x14021d21e`。
- **所有权结论**：游戏只在 bit62 未置位时释放；无 refcount。
  → 我们的实现**必须把头 word 的 bit62 置位**，内存自持（否则游戏会用它自己的分配器 free 我们的指针）。
- 释放时机安全：`handle_request_compile_and_run` 每条请求都先 `send_command(3)+joinThreads`
  再走到 `compile()`（✅ `0x14021d230` 段），且 `compile()` 只有一个调用点、在单线程串行
  → 「下次 compile 开始时释放上次的产物」安全；最保守也可直接泄漏。

### 2.4 游戏侧执行与程序模型（✅）

- `jit__modelZsimulationZjitZjit_u1807` @`0x14021cc60`：
  `CreateFileMappingW(len, PAGE_EXECUTE_READWRITE=0x40)` + `MapViewOfFileEx`（🔍 这两个是 dynlib
  取到的函数指针，T4 判读；语义已由调用序列 + `memcpy` + `jmp` 证实）
  → `memcpy(dest, out[1]+8, len)` → `c_jit()`（✅ 实测 `0x140001a40` 就是 `ret`）→ `jmp *dest+entry`。
  **不设置任何寄存器、无参数、无返回值**。
- `jit_function` @`0x14021cd40` 在调用 `jit` 之前对产物做一次 Nim 字符串拷贝
  （`eqcopy` @`0x14021cd78`）→ `jit` 的 memcpy 源是那份拷贝（或经 strlit 优化退化成别名）。
- **entry 每次编译恰好进入 1 次**（✅ T3）：`jit` 尾跳后无返回路径；`jit_function` 全 exe 只作为
  `createThread` 的线程过程出现一次；`handle_request_compile_and_run` 每次请求都无条件编译 + 起线程。
  触发单位 = 一条 sim 请求（单步 / 输入变化 / 关卡测试 / 关卡加载 / 重编译各 1 次）。
- **程序根 = 顶层语句块**（✅）：空源码 / 只定义函数 → 产物恒 8581 字节（函数被 DCE）；
  顶层写一句 `store(...)` → 产物 +13 字节且立即数落在 `0x215c`。顶层块末尾有隐式 `thread_exit()`。
- 生成代码的顶层块是长驻主循环（`work/hook/hook_src_001.si:1058`，全文 1182 行）：
  `run_sim: while true { reset_sim(); while true { mode_refresh(); …轮询命令… } … switch 命令 { case run { mode_run(target_cycle) } … } }`。
- **推论**：entry 语义 = 「一次运行」；每次进入都重新初始化（blob 内数据每次被 memcpy 覆盖）；
  跨请求状态只能放在产物之外（游戏内存 —— 生成源码里本来就是写死的绝对地址）。

### 2.5 原版编译器的产物质量与编译延迟（✅）

- RV32IM 产物（46591 条指令）：`mov*` 占 65.2%（其中 reg-reg 拷贝 27.7%）、碰 `rsp/rbp` 19.4%、
  置零 6327 次；典型片段反复重算地址：`mov %rax,%r9; add $8,%r9; mov %r9,%r8; mov (%r8),%r9`。
  ⚠️ 分母是**最初 16 份**语料（未按 180 份 / 去重 98 重算）；重算属 M0 任务。
- 纯窥孔改写（去掉变量移位依赖链）已实测 **5.4 → 8.0 MHz（+48%）**。
- 编译延迟：419888 B 源码 → 458276 B 码，**≈2.09 s**。设计预算：最终 **5 s**（调试期放开）。
- 参考编译器结构（🔍 由 `recovered/compiler-src/OO/*` 符号表）：66 模块 / 1989 函数，
  管线 front_end（parse/infer/generics/macro）→ middle（SSA/phi/peephole/suffix_tree）→
  back_end（lifetime/allocate_registers/spill_sort/emit）→ targets(x86_64/aarch64) + jit。

### 2.6 固定前缀（stdlib）与内建清单（✅）

- `compile()` 每次调用都把 **27711 字节 Simplex 源码**（`compile_real.dll` 偏移 `0x119F48`，1503 行）
  拼在用户源码之前，再整体编译。口径：27711 字节**含 55 处 CRLF**；工作副本 `stdlib_prefix.si` 已做
  CRLF→LF 归一化，所以是 27656 B —— 两者等价，不要去“修”这个差。该前缀是**游戏作者的运行时库**：类型/常量、arena 内存管理、
  字符串与打印、数组与排序、大整数、时间与随机、`extern windows_x64 kernel32` 系统层。
- 从前缀里「被调用但未定义」的名字可抽出**编译器内建（intrinsic）清单**（约 20 个）：
  `load, store, raw_cast, empty_array_address, grow, shrink, join, index_read, index_write,
  clz, inc, swap, type_of_element, partition, big_int_sub/mul/div_u/div_s/mod_u/mod_s/rol/ror/asr/bswap`。
  ⚠️ 该清单由生成源码 + 前缀反推得出（分母是最初 **16 份**；语料已扩到 180 份 / 去重 98，见 §2.7，
  但清单未按新语料重算），**仍是派生物而非冻结规格**。
- `print_using_stack` 在 x86-64 Windows 上的实现被作者注释掉了（注释原文 `X86 WINDOWS IS UNSOUND SOMEHOW`），
  只剩 aarch64 的 `svc 0x80` → Windows 上 `print` 很可能是空操作（🔍，待验证）。

### 2.7 语料（✅）

| 来源 | 规模 | 说明 |
|---|---|---|
| 官方关卡测试代码 | 92 文件 / 464664 B（454 KB） | **在游戏安装目录**（`F:\opt\SteamLibrary\steamapps\common\Turing Complete`），不在仓库；需拷进 `simplexc/corpus/` 固化 |
| 转发层抓取的生成源码 | `work/hook/all/`：180 份 `.si`（29 MB 含产物） | **去重后 98 份唯一源码**；最多一份重复 73 次（“地图电路”，切换关卡会重复加载） |
| 生成源码的参考产物 | 180 份 `.bin`（与 `.si` 同名配对） | ⚠️ **`work/hook/all/` 里没有 manifest**（转发层每次加载都清 `work/hook/`；现存 `manifest.tsv` 只有最近一次会话的 9 行）→ 180 组只有「源码, 产物」，**entry 一个都没留**（entry 只在 manifest 的 `r2` 与 `NNN.entry.txt`，两者都已丢） |
| stdlib 前缀 | 1 份 / 27711 B | 已提取（工作目录 `stdlib_prefix.si`） |

生成源码用到的构造：顶层语句、`load/store`（9.4k 处）、sized int 与强制转换、`Struct/Enum/别名`、
`switch/case`、`while`、带标签循环、字符串插值、`@Size/@Type` 泛型、`raw_cast`、
`#if in_scope("fn"){…}`（编译期条件）、`in_scope`。

**M0 的任务**：用 `tools/try_compile_dll.py` **离线重编译 98 份唯一源码**（离线编译确定性，见 §2.1），
生成**含 `entry` 的 manifest** → 再按“用到的构造”打标签 → 覆盖率矩阵；把“沙盒模式用到的构造集合”
标为**必须支持**（MVP 验收下界），其余标为“可回退”。**目标不是覆盖语料全部构造** —— 有些构造可能
只有主线关卡会用到。

### 2.8 社区前作（✅）

`github.com/michalusio/TC-si`（MIT，9.4k 行 TypeScript）：Simplex 的解析器 + 类型检查 + 标准库签名表 +
代码生成 + 寄存器分配 + 优化器，目标为游戏内 Symphony 架构（不是 x86-64）。覆盖 `Struct/Enum/泛型/
运算符重载/asm/extern`；**不覆盖** `#if in_scope`、`raw_cast`。可作前端语义参考，不能当后端复用。

---

## 3. 系统框架

```
游戏进程
  Turing Complete.exe（Nim）
    compile 线程 ──(1) compile(out, src, flags=0)──▶ compile.dll（我们的壳）
                                                      │
                       ┌──────────────────────────────┴───────────────────────┐
                       │ 0. 壳：加载 compile_real.dll（回退用）                 │
                       │    模式：ours / ours_with_fallback / reference        │
                       │    记录 flags 与 Result（转发层已有 manifest.tsv）     │
                       └───────┬───────────────────────────┬──────────────────┘
                       失败/不支持 │                           │ 支持
                                   ▼                           ▼
                        compile_real.dll（原样返回）   我们的编译器
                                                        ├ 前端：词法/语法/预处理(#if in_scope)
                                                        │      作用域/类型/泛型/常量求值/重载解析
                                                        ├ Lowering：Simplex 语义 → LLVM IR
                                                        │      （load/store 直映射、别名、$ 写回、
                                                        │        内建映射、extern 声明、entry 包装）
                                                        ├ LLVM 管线（`LLVM-C.dll` + C API，版本锁定）→ 目标码
                                                        └ 装配器：局部符号 PC 相对 + extern 解析 +
                                                           8 字节头(bit62) + entry 偏移 + 生命周期
                                                      │
    jit 线程 ◀──(2) 分配可执行内存 → memcpy(blob+8, out_len) → jmp base+entry ──┘
    （entry 内部自带主循环；每次编译 = 新 blob + 新线程 + 进入 1 次）

验证支线 A（离线，agent 日常）：
  语料 → 参考产物 ┐
  语料 → 我们的产物        ┼→ 差分宿主（假游戏内存 + 命令驱动 + extern 桩）→ 逐 tick 状态 diff
                          ┘
验证支线 B（游戏内，验收）：
  真实关卡 + RV32IM 电路 → 转发层切 ours/reference → watch_freq.py 采样主频
```

**同一个核心，两个前端**：**DLL 壳**（游戏内进程调用）与**离线 CLI**（`simplexc-cli --src x.si --out blob.bin`）。
agent 日常、差分宿主、回归测试全走离线 CLI，不碰游戏；壳只是薄薄一层。

### 3.1 组件职责

| 组件 | 输入 → 输出 | 职责 | 现状 |
|---|---|---|---|
| 转发层（壳） | 游戏调用 → 产物 | 选实现、记录 `flags`/Result、异常熔断（绝不让游戏崩）、失败转交 | **既有 `tools/compile_hook.c` 演进**（它已承担 manifest/捕获/purge/配置读取/entry 回写）；`simplexc/shell/` 只放“选实现 + 失败转交 + 核心入口”这段新代码，契约只有一份 |
| 前端（`frontend/`） | 源码文本 → **无类型 AST** | 词法/语法、`#if in_scope` 的裁剪点、span | **已落地**（M4.5） |
| sema（`sema/`） | 无类型 AST → **HIR** | 名字解析、类型、常量求值、泛型单态化、内建识别、根确定与 DCE；**不发任何 LLVM 指令** | **已存在**（现为 `lower.cc` 内 27 函数/651 行，待搬出） |
| codegen（`codegen/`） | HIR → LLVM IR | 语义保真映射（§5）、内建映射表、extern 声明、entry 包装 | **已落地**（现为 `lower/`，待改名） |
| LLVM 管线 | LLVM IR → 目标码 | 固定版本 + pass pipeline + 目标三元组/ABI | **已定**：`LLVM-C.dll` + C API（`LLVMRunPasses` + `LLVMTargetMachineEmitToMemoryBuffer`）；三元组 `x86_64-pc-windows-gnu`；链接与构建方式见 §8 |
| 装配器 | 目标码/目标文件 → blob | 局部符号 PC 相对、extern `GetProcAddress`、8 字节头、entry 偏移、blob 生命周期 | **可用**（`assemble/`；含桩/槽两趟定址与 sidecar 自校验） |
| 离线 CLI | `.si` → blob 文件 | 与壳共用核心；给 agent、差分宿主、回归用（不碰游戏） | **可用**（`cli/tcblob`、`cli/llvm_prog`、`cli/prog_empty`） |
| runtime | — | 生成代码/测试代码真正用到的内建与运行时（路线 B）；终态由 stdlib 源码取代（路线 A） | 只有 `rt_min.cc`（spike 用） |
| 验证体系 | 语料 → 判据 | 语义档案、离线差分宿主、游戏内 A/B、判据入口 | 待写 |

**语言分工**（**唯一权威清单在接口冻结版 §0**；这里只给理由与被禁的做法）：

- **产品路径（在游戏进程内跑）= C++17**：`frontend/`、`sema/`、`codegen/`、`runtime/`、`assemble/`、`shell/`、`cli/`
  （必要时用汇编，如 `shell/entry_shim.S`）。理由不是产物速度（产物质量由 LLVM 决定），而是
  ①编译延迟预算 5 s（§10）②drop-in 部署（DLL 自包含，不带 Python 运行时）
  ③崩溃面（动态语言跑在游戏进程里，一崩就带崩游戏）。
- **离线工具 = Python**：`tools/`（语料、参考产物生成、差分宿主、判据入口、freq/bench）。它们不进产品路径。
- **产品路径不得依赖 Python**：DLL 及其加载的组件不得链接或调用 Python，由 §7.5 的「依赖门」检查。
- **历史**：M3 期的 Python 原型已于 2026-10-18 删除（git 历史可查），**不再有任何实现被当作语义权威**。

### 3.2 分层与职责（2026-10-18 重构定）

| 层 | 输入 → 输出 | 只做这些 | 不许做 |
|---|---|---|---|
| 前端 `frontend/` | 源码 → 无类型 AST | 词法、语法、`#if` 的**结构**（解析出 `PpIf` 节点）、span | 类型推导、泛型实参解析、可达性判断（编译路径）、`in_scope` 判定 |
| sema `sema/` | AST → HIR | 名字解析、类型、常量求值、泛型单态化、内建识别、根确定与 DCE | 发任何 LLVM 指令 |
| codegen `codegen/` | HIR → LLVM IR | 语义保真映射（§5）、extern 声明、entry 包装 | 回头查源码；做类型推导；决定“要不要实例化”（实例集合已由 sema 定死） |
| 装配 `assemble/` | 目标文件 → blob + sidecar | 重定位、桩、8 字节头、行号表重映射 | 改语义 |

**`#if in_scope` 归层（审查更正）**：前端只解析出 `PpIf` 节点；**判定与删除子树在 sema**（依赖作用域，
当前实现在 `lower.cc` 的 `s_PpIfStmt`/`ppif_true`，:1089-1094）。不要把 `in_scope` 判定放进前端——那里拿不到作用域。

**HIR 六条不变量**（codegen 据此可直接发指令，不必回头推理）：

1. 每个节点带解析后的类型（没有“待定”类型）；
2. 每个名字已绑定到具体声明（没有“待查”符号）；
3. 泛型与闭包实例已展开（没有“待实例化”）；
4. 隐式转换已显式化（截断、符号扩展、位宽提升都是独立节点）；
5. 每个节点带源码 span；
6. 实例键唯一且已规范化（同一 `(def, 类型实参, 环境类型)` 只出现一次）。

**不自研 MIR**：LLVM IR 就是本项目的 MIR（SSA + CFG + 类型）。优化是 LLVM 的职责；我们只在 HIR 上做语义确定。

**依据（可复现，2026-10-22 审查修正）**：`lower.cc` 3428 行 / 175 个顶层函数；其中**函数体内不出现 `LLVM*`
的 94 个（1545 行）**，按 sema 特征（`ty_*`/`make_*`/`fold_*`/`static_type`/`unify`/`arith_ty`）筛出 53 个
（1010 行，29.5%）。最大的 `static_type`（1713–1838，126 行）体内 `LLVM` 出现 0 次，签名是 `Node → TyRef`。
所以分层的实际工作是“把这些函数搬出去 + 拆开 `struct V`”，不是重写。
（早前版本的「27 函数/651 行/19%」无法重建，已废。）

### 3.3 状态与所有权（2026-10-18 重构定）

**问题**：`FnState` 10 个字段靠 `save_state()`/`restore_state()` 逐字段手工存取（lowering 一个 def 的过程中
会递归 lower 另一个 def）；加一个字段要改 3 处、编译器不检查；已产出两起错序 bug。

**约定**：

- **sema 上下文**（符号表、类型表、实例表；可共享/不可变的部分）与 **codegen 上下文**（每函数新建的
  builder、基本块、局部绑定）**分成两个对象**；
- 递归下降时**新建子上下文或以参数显式传递**；**禁止**保存/恢复可变成员 —— 让“忘了恢复”从“可能发生”
  变为“无法表达”；
- `struct V{ll, ty, c}`（IR 值 + 语义类型 + 常量三合一）是分层要拆的耦合**之一**；同类还有
  `Capture{name, ptr, ty}`、`Binding{ptr, ty, value, def}`、`FnState`（均有 LLVM 值 + 类型）。
  分层后 **sema 的 V 不带 `ll`**，codegen 用自己的值表示并从 HIR 读类型；
- 不变式用 `assert` 强制（当前全仓 0 个）。

### 3.4 错误模型（2026-10-18 重构定）

三条**独立**的轴（现在压成一维，导致“用户写错”被统计成“我们的 bug”）：

| 轴 | 取值 | 用途 |
|---|---|---|
| 责任 | 用户源码错 / 未实现 / 我们的 bug | 决定该谁修；回退率的分母 |
| 阶段 | 词法 / 语法 / sema / codegen / 装配 / 壳 | 决定往哪查 |
| 可定位性 | span（文件+行+列）/ 只有行 / 无位置 | 决定错误消息是否有用 |

**约定**：

- 单个错误类型 `CompileError{responsibility, stage, span, detail}`，从第一层就带**全部三轴**
  （**目标**：当前 `Result` 五字段里只有 `out[3]/out[4]` 放消息，`stage` 只进 `hook.log`；责任与位置无处放）；
- `span` 一路传到 C 边界；过边界时**不许只剩类别名**（现在 `catch(...)` 全部折成 `InternalError`）；
- **禁止静默降级**（目标）：未识别构造必须显式失败并归类。已知 6 条静默路径必须消除：
  ①未识别语句关键字掉进赋值/表达式解析（`parser.cc:344`）；②解析期 `notes` 只进 `stats.warnings`、
  无消费者（`frontend.cc:420-423`）；③`compile_real.dll` 加载失败不写任何记录（`compile_hook.c:356-358`）；
  ④`scan_decls` 对 struct 字段只 `catch (Unsupported&)` 记一条、继续执行（`lower.cc:3316-3318`）；
  ⑤顶层未知行可能被解析成合法表达式而仍产出 blob（`parser.cc:292-311`，待确认）；
  ⑥「不可达 def 里的语法错不报」——审查**未定位**判定点，暂记待查；
- 回退率按三轴统计 —— 这是“何时能关回退”的唯一量化依据。

---

## 4. 产物契约（必须逐字复现）

| 项 | 值 | 依据 |
|---|---|---|
| `out[0]` | **缓冲总长（含 8 字节头）** = `8 + 码长`（与原版编译器同形，工具链不变） | ✅ 2.2 |
| `out[1]` | 缓冲指针（我们分配、我们持有） | ✅ 2.3 |
| `out[2]` | `entry_off`，**相对机器码起点**（= `blob+8`） | ✅ 2.2 |
| `out[3]`/`out[4]` | 成功必须 `0`/`0`；失败给 `{len≠0, ptr}` 的 Nim 字符串形状 | ✅ 2.2 |

> 失败**不自造**错误消息 —— 整单转交原版编译器并原样返回它的 Result（§3）。若将来自造，才需要上面这个形状。
| `blob[0:8]` | 小端 u64 = **`out[0] \| (1<<62)`**（bit62 = 不要 free） | ✅ 2.3 |
| `blob[8:]` | 机器码；内部引用一律 PC 相对 | ✅ 2.3 / 🔍 |
| 缓冲实际分配 | **`out[0] + 8` 字节**（多 8 字节 padding） | 见下 |
| 释放 | 由我们负责：每次编译分配一块，**下次编译释放上一块**（或泄漏） | ✅ 2.3 |

两条容易踩的点：

1. **多分配的 8 字节 padding 是必需的**：游戏 `memcpy(dest, blob+8, out[0])` 会读到 `blob+8+out[0]`，
   即比缓冲末尾多 8 字节（✅ 依据：`0x14021cc89 mov %ecx,0x20(%rsp)` 把 `out[0]` 当 section 大小、
   `r9d=0`、`0x14021cce9 memcpy(dest, out[1]+8, out[0])`）。原版编译器靠 Nim 字符串的分配余量兜住；我们必须显式留出，
   否则可能读到我们自己分配的页边界外（我们用的是自管内存，不像 Nim 字符串有对齐余量）。
2. **bit62 会让"头值 == 文件总长"这条工具链约定失效**：`tools/analyze_jit_code.py` 的 `header_len`
   自检要求头值等于文件总长。工具链比较时应掩掉 bit62（`value & ~(1<<62)`），
   或明确接受"头值 = 总长 | bit62"。这是我们要改的唯一一处既有约定。
3. 头 word 里**不放别的东西**（不放 magic/版本）——需要元数据就放 padding，别动头。
4. **原版编译器的产物 bit62 = 0，我们置 1 —— 这是有意的分歧**（✅ 180 组实测：`work/hook/all/hook_out_*.bin`
   的头 word 低 32 位 == 文件长度，bit62 全为 0）。理由：原版编译器是 Nim，free 走 Nim 分配器；
   我们用 malloc 分配，若 bit62 = 0，游戏会用 Nim 的 `free` 释放我们的指针 → **跨堆释放会崩**。
   置 1 后游戏不 free，改由我们在下次编译时释放上一块（§2.3 的所有权结论）。此点列入 spike 验收。

### 4.1 extern 与重定位（设计约束）

- extern（`kernel32` 等）在**编译期**解析成进程内绝对地址（✅ 原版编译器如此）。
  ⚠️ 2026-10-16 更正：那些 `movabs` 指向的是 **`compile_real.dll` 自己的导入 thunk**（DLL 内 RVA `0x1310`
  = `jmp QWORD PTR [rip+0x147059]` → `.idata`），**不是 kernel32 自身的地址** —— 原文「全落在 kernel32 区间」
  是地址区间的误读。后果（尤其是「产物跨 boot 不能逐字节相等」）见 `docs/facts.md`
  §「产物的绝对地址绑定 compile_real.dll 的加载基址」。
- 装配器职责：局部符号解成 PC 相对；外部符号用 `GetProcAddress` 解析。若目标地址超出 `call rel32`
  范围，需在 blob 内生成 `movabs rax, imm64; jmp rax` 桩（stub pool），不能让重定位失败。
- 产物内部**不得**出现指向自身固定地址的绝对引用（blob 会被游戏拷到任意地址执行）。
- **目标三元组 `x86_64-pc-windows-gnu`**（与游戏的 MinGW/Nim 构建一致），extern 调用必须满足 Win64 ABI
  （rcx/rdx/r8/r9 + 影子空间 + 栈 16 字节对齐）；原版编译器调用 kernel32 的序列可反汇编核对（U10）。
- **装配器的输入契约（spike 必卡项，先写死）**：`LLVMTargetMachineEmitToMemoryBuffer` 给的是 **COFF 目标文件**，
  装配器必须解析节/符号/重定位（仓库已有 `tools/coff_symbols.py` 可复用，别从零写）。三个已知坑：
  (a) LLVM 默认给函数发 **`.pdata`/`.xdata`**（unwind，含 `ADDR32NB` 类重定位）→ 要么在 IR 层禁掉
  `uwtable`/异常，要么整节丢弃；(b) 栈帧 > 4 KiB 时会自动 `call __chkstk`（**CRT 符号，不是 kernel32**）
  → 必须给 stub 或限制帧大小；(c) 未定义符号一律按“stub pool + 运行期解析”处理，不许重定位失败。

---

## 5. 前端必须给 LLVM 什么（语义保真）

主频提升主要来自 LLVM 后端，但**前端决定 LLVM 能做什么**：

1. **`load`/`store` 必须 lowering 成 LLVM 的 load/store**（宽度与对齐按 §5.1 的实测规则），
   **不能**降成对 runtime 函数的不透明调用 —— 生成代码里 9.4k 处 `load/store`，
   这正是参考产物 65% 是 `mov` 的根源之一。注意：语言不保证对齐、也不能声明 TBAA（§5.1）。
2. **`$` 参数写回**、`raw_cast`、指针别名必须给出**精确**（不保守）的 IR。
3. **extern 调用的副作用要标注**（无副作用者可让 LLVM 跨调用优化，有副作用者必须挡住）。
4. **函数边界要干净**：参考产物是"多函数拼接 + 数据混在代码流里"，会让内联与寄存器分配退化。
5. **顶层块**（程序的 `main`，长驻循环）要作为正常函数参与内联/优化。
6. **常量与泛型实例化在编译期完成**（大整数常量、`#if in_scope` 分支裁剪），不留运行期分支。
7. **未支持构造必须显式报错并回退**，不允许“猜一个语义”。
8. **最外层入口必须是我们手写的 shim**（保存全部寄存器 + 把栈对齐到 16 字节 + `call` LLVM 生成的函数）：
   游戏是 `jmp` 进来的，不保证 ABI 对齐；LLVM 生成的代码按 ABI 假设对齐（对齐的 SSE 指令会 fault），
   所以 LLVM 生成的函数**不能**直接当最外层入口。补充三点：
   - 入口处**没有任何寄存器被清零**（实测尾跳 `lea (%rsi,%rdi,1),%rax; jmp *%rax` → 进来时
     `rdi=entry`、`rsi=dest`、`rbx=Result*`）；shim 不得假设寄存器为 0。
   - 线程栈大小与递归深度预算**未知** → shim 与生成的代码不得依赖大栈。
   - **熔断机制**（“绝不让游戏崩”）首选 **VEH**（`AddVectoredExceptionHandler`，纯 Win32 API、与编译器
     无关）；MinGW GCC 在 x64 上对 `__try/__except` 的支持需实测。要写清熔断后**能否继续**跑
     （建议：熔断后本进程内退回 `reference` 模式）。

### 5.1 内存与优化假设（2026-10-18 定）

必须把**两件事分开**（现在混在一起，是“主频上不去”与“不敢优化”的共同来源）。

**(a) 语言语义 —— 已实测，不可协商**

| 项 | 事实 | 证据 |
|---|---|---|
| 地址单位 | **字节地址**；`p + 1` 是下一字节，不乘元素宽度 | U-L1 |
| 对齐要求 | **无**；非对齐读写静默成功 | U-L1 |
| `store` 宽度 | `N ≤ 32` → `ceil(N/8)` 字节；`N > 32` → **8 字节零扩展写**，越界清掉 `8 − ceil(N/8)` 个邻居字节（U33–U40 清 3、U41–U48 清 2、U49–U56 清 1、U57–U64 清 0；实测集到 U48） | U-L1（1..64 穷举） |
| `load` 宽度 | 与 `store` 对称；读后按 `N` 位掩码、向上零扩展 | 同上 |
| `N < 8` | **整字节访问**（预填 `0xFF` 写 0、读回 `0x00`），不是读-改-写 | 同上 |

→ 直接约束 codegen：`align` 只能声明 1；`N > 32` 的写必须真的写 8 字节（“优化”成 5 字节会改邻居语义）；
`N < 8` 可以发 `store i8`。

**(b) 向 LLVM 声明的假设 —— 可开关，默认最保守**

| 假设 | 取值 | 理由 |
|---|---|---|
| `align` | 恒 **1** | 语言不保证对齐，声明更大即 UB |
| `noalias` | **永不声明** | 游戏与我们共享内存，随时可能被外部改 |
| TBAA | **不用** | `.si` 的类型不构成 LLVM 层面的别名保证；访存本质是“地址 + 位宽” |
| 区域独占 | **按区域声明** | 唯一能安全拿到的优化信息 |

**区域模型**：别名信息的来源不是“类型”，而是“区域”。每个区域声明“谁写、谁读”，优化只对我们独占的区域做激进变换：

| 区域 | 谁写 | 当前可否假设独占 |
|---|---|---|
| 游戏状态段（`#SIMULATION_STATE` 等） | 游戏与我们 | 否 |
| RAM 镜像（`#DATA_*`） | 双方 | 否 |
| 我们的栈 / arena | 只有我们 | 是 |
| 未来：控制段重分配 | 未知 | **由布局描述决定** |

**布局是输入，不是常量**：游戏内存地址现在写死在生成源码里；若将来另有项目改变控制段分配，
只换区域描述，codegen 与优化不用动。

### 5.2 实例化（泛型 / 闭包）

**问题**：现在在 codegen 过程中“边生成边决定要不要发实例”，捕获签名**按调用点摊平**，
注释自述“名字集合的幂集可能无界”，于是 `instances >= 64` 报错回退 —— 把架构缺陷写成了语法限制。

**约定**：

- **在 sema 阶段完成全部实例化**；codegen 看到的 HIR 里没有“未定类型”、没有“待实例化”。
- **实例键** = `(def 名 + 定义行号, 类型实参列表, 环境类型签名)`，**规范化后**比较（排序、去重、别名归一）；
  **不含调用点** —— 这是当前 bug 的根源。现状（`lower.cc:2561-2562`）键含 `lo`，但 `cap_sig` 直接拼接、
  未规范化。
- **闭包转换已是现状**：捕获本来就按显式 `ptr` 参数传入（`lower.cc:2605/2608/2656`），不需要重做；
  待做的是键规范化与「实例化提前到 sema」。
- 待实例化用**队列**推进（遇到新键入队，处理到队空），避免递归爆炸。
- 每个实例**记录来源**（哪个 def、哪些实参、哪些捕获）→ 供热点归因。
- 真有无限递归泛型 → **显式报错**并按 §3.4 归类，计入回退率。

### 5.3 为什么禁止转译

- sized int 的精确 wrapping/隐式截断、`$` 参数写回时机、数组越界与别名语义，在 C/C++ 里无法精确表达
  （引入 UB 或语义漂移）。
- 内建（`index_read/index_write/grow/…`）语义无法在 C/C++ 层一一对应。
- 语义在转译层就丢了一次，之后连差分测试都定位不了。

---

## 6. runtime 与 stdlib：两条路线

| | 路线 B（先做） | 路线 A（终态） |
|---|---|---|
| 做法 | 前端不碰前缀，自研 runtime 覆盖生成代码/测试代码真正用到的内建与运行时 | 把 27711 字节前缀也交给我们的前端 |
| 真实差别 | **增量**：不支持的程序回退原版编译器 | **全有全无**：前缀对每次编译都生效，少支持一个构造 → 所有编译全挂 |
| 代价 | 必须实现运行时**行为等价** | 前端必须先覆盖全部语法（泛型/重载/asm/extern/大整数常量） |
| 判据 | 差分测试 + 覆盖率矩阵 | 同上，且覆盖率矩阵覆盖 stdlib 全部构造 |

两者都必须吃透 stdlib 语义；差别只在"必须先吃透全部"还是"边跑边吃"。**先 B 后 A。**

---

## 7. 验证体系

### 7.1 差分测试（唯一正确性判据）

> 同一份源码，分别由原版编译器与我们编译；运行后**对状态的改变必须逐 tick 完全一致**。

- **可观测量**（已决定）：= 程序写过的游戏内存页 ∪ extern 调用的实参/返回值序列。
  游戏内显示器 / UI / 输出组件都源自内存写（🔍 输出组件很可能只是对特定地址的写），天然被覆盖；
  不依赖宿主机输出。
- **白名单**：arena/分配器内部布局等不可观测差异必须显式列白、可审计、尽量小。
- 逐 tick 观测不需要插桩：主循环是 `case run { mode_run(Int get_setting(sim_target_cycle)) }`，
  驱动方把 `sim_target_cycle` 依次设为 1,2,3,… 并递增命令序号发 `run`，即得天然 tick 边界。

### 7.2 离线差分宿主（agent 日常）

- 复用游戏的执行方式：分配可执行内存 → `memcpy(blob+8)` → `jmp base+entry`。
- **假游戏内存**：生成源码把所有绝对地址写死在源码里（`Ptr 1781626765312` 这类），扫出来按页分配；
  更稳的做法是守卫页/缺页记录触达地址。
- **extern 中和**：参考产物的 kernel32 地址是编译期写死的绝对立即数 → patch 成我们的桩
  （`ExitProcess/ExitThread/sleep/WriteFile`），否则宿主进程会被杀。
  `tools/optimize_image.py` 已有 movabs 改写能力可复用。
- **自比对基线**：先跑「参考产物 vs 参考产物」证明宿主本身可靠，再接我们的产物。
- 超时 + 可重放（程序内含 `while true` + `sleep`）。

### 7.3 游戏内 A/B（验收）

- **M1.5 收益探针（提前做）**：spike 之后先拿一块最小电路做端到端 A/B（参考产物 vs 我们的产物），
  量出主频提升的量级；否则可能前端做完才发现收益不够。
- 转发层支持切换实际编译器（`ours` / `ours_with_fallback` / `reference`）。
- 主频底层 = 读游戏内存（已确认、已做好）；benchmark 固定 RV32IM 电路；协议：多轮取中位数、同一次游戏会话内交替。
- **主频/bench 这一层已重写立项并定稿**：设计见 `docs/specs/2026-10-15-freq-measurement-design.md`。
  归属语义在这里统一（三条线含义不同，别混）：`cfg` = **配置了什么**、`opt` = **改写器实际做了什么**、
  新增的 per-call `impl` 行 = **实际走了哪个实现**；**归属只认后两者**。落地时同步 `facts/forwarding.md`
  与 `tools/hook_log.py` 的事件表（搬 `hook.log` 时一并改）。

### 7.4 语义档案（semantics dossier）

每条 = `{名字, 最小 .si, 驱动方式, 参考产物下的可观测状态变化, 备注}`，**机器可执行**、
由脚本自动采集（写一条 → 跑一次采集 → 入库）。覆盖：cast 顺序、wrapping、移位越界、除零、
数组越界、字面量宽度推断、泛型实例化、`#if in_scope`、struct 布局/对齐、enum 取值、
`$` 写回时机、字符串编码。每条同时就是一个差分测试用例。

**第一批（打底 12 条）**：cast 顺序/截断、wrapping、移位越界、除零与取模、有符号比较、数组越界、
struct 布局与对齐、enum 取值、字符串字面量编码、`$` 写回时机、整数字面量宽度推断、`switch` 与带标签 `break`。

### 7.5 判据入口

一条本地命令跑完：`解析全语料 → 全量编译（我们 vs 参考）→ 逐 tick 状态差分 → 覆盖率矩阵
（特性 × 文件）`，输出通过数、失败的最小复现、未覆盖特性（机器可读报告 + 人读摘要）。
作用：让并行 agent 能自证“做完了”。游戏内部分单独标 manual/slow，不进这条命令。

**必须同时给出回退率**（per-corpus + 原因分类：未支持构造 / 内部错误）——它是“何时能上路线 A、
何时能关回退”的量化依据；只有覆盖率矩阵不够。

**另两条门（2026-10-17 定）**：

- **依赖门**：构建产物 `build/compile_hook.dll`（部署到游戏目录时才改名 `compile.dll`）的导入表里
  **不得出现 `python*.dll`**：`objdump -p build/compile_hook.dll | grep -i python` 必须为空。
  补充弱检查（不算强保证：静态导入表查不到 `LoadLibrary` 动态加载）：`grep -a -i python build/compile_hook.dll`。
- **延迟门**：最大语料 `corpus/src/177.si`（✅ 479225 B ≈ 468 KiB）端到端（前端 → lowering → `-O2` → 装配）
  **≤ 4 s** —— 留 1 s 余量给壳的硬超时（`ours_timeout_ms` 默认 5000 ms），并单独记「前端 + lowering 占多少」。
  **同输入对照**：用 `python tools/try_compile_dll.py --compile --src simplexc/corpus/src/177.si` 在同一条文件上
  掉表量原版编译器（§2.5 的 2.09 s 是 `001.si` 419888 B，不是同一条文件）。
  统计前必须把判据入口的生产者切到 C++ 前端产出路径（否则会验错对象）。

### 7.6 判据体系与调试信息（2026-10-18 重构定）

**现状问题**：前端没有判据步骤；`lower.cc` 175 个顶层函数，仓库**没有方法级最小用例**
（无 `simplexc/tests/`；只有 `work/tmp/dossier/exp_paren/` 的 11 个实验 `.si` 与 `m3_0_constructs.tsv` 的
143 行构造检测表，二者都不是最小用例）；全仓 0 个 `assert`；98 份语料卡在 2–3 个前置阻塞点，
覆盖率矩阵**无信息量**。

**判据约定**：

- **模块接缝**：每层都能单独喂输入、单独验输出（sema 输入 = AST，可由文本构造；输出 = HIR，可比对）。
- **最小用例**：每个 sema / codegen 功能点一个最小 `.si` + 期望，不靠大语料间接测。
- **覆盖率口径**：**先解锁前置阻塞再统计**；按（构造 × 语料）出矩阵；同时给回退率（按 §3.4 三轴）。
- **单测 vs 差分的边界**：**不变式用 `assert`/单测；语义用逐 tick 差分**。两者不互相替代。
- 语言语义的每条（U-Lx）必须同时是：①实测记录 ②一条差分用例 ③sema/codegen 的断言。

**调试信息**（性能归因的唯一入口）：

- 产物旁 **sidecar**（不进 blob），提供「**偏移 → 行 → 标识**」，且**不规定执行单元如何划分**。
  **现状**：只有 `AsmResult::stubs{offset, symbol}`（`assemble.hh:97-101`），下列内容全部待做；
  与 `out_len` 无冲突（`out_len = Blob::bytes.size()`，sidecar 不在其中）。
- 内容（**目标**）：机器码区间 `[start,end)`、源码行区间、所属 def / 实例键、可选调用点。
- 实现路径：codegen 设 debug location → 编译开 `-g` → 装配器解析行号表并**重映射到 blob 偏移**。
  ⚠️ 未测：行号表在 COFF 里的形态、`llvm-dwarfdump --debug-line` 能否直接当解析器。
- 元件级归属由**下游**按源码里的元件注释把“行区间”翻成“元件 + 实例”；注释格式有坑
  （`ID=0` 的元件不受“两处”约束、字段数可变、有 `LATE` / 负 ID）——
  **所以设计不规定怎么切**，只在 sidecar 里给“行”。
- 另记：opt level、pass pipeline、五段耗时（前端/sema/codegen/LLVM/装配）写进 sidecar，
  否则“LLVM 化了但没变快”无法归因。

---

## 8. Agent 工作流与接口冻结

**接口以 `docs/specs/2026-10-15-simplexc-interfaces.md`（接口冻结版 1.4）为唯一权威** —— 本节只定分区与纪律，
不复制契约内容；两者冲突以接口冻结版为准（改接口要走那份文档的版本号登记）。

实现几乎全部交给 agent，因此**先冻结接口、再并行开工**。代码落库位置已定：`simplexc/`（2026-10-15）：

```
simplexc/
  frontend/   词法、语法、ast（无类型）、预处理(#if in_scope 的裁剪点)、span
  sema/       名字解析、类型、常量求值、泛型单态化、内建识别、根确定与 DCE → HIR
  codegen/    HIR → LLVM IR（内存模型、$ 写回、extern、entry 包装）
  runtime/    路线 B 的运行时（arena、字符串、打印、排序…）
  assemble/   目标码 → blob（PC 相对、extern 解析、8 字节头、entry、生命周期）
  shell/      壳里**新增的那部分**（选实现、三模式、失败转交、核心入口）——
              既有的 `tools/compile_hook.c` 继续演进，**不做第二份壳**
  cli/        离线 CLI（`--src/--out/--entry`），与 shell 共用核心
  tools/      语料抓取、参考产物生成、差分宿主、判据入口、语义档案采集
  corpus/     语料 + manifest
  dossier/    语义档案（机器可执行）
```

**必须先冻结的接口**（冻结前不要并行开工）：

1. **前端产物**：无类型 AST（节点种类、span）；**sema 产物**：HIR 与类型表示（sized int 宽度/符号、struct 布局规则、泛型实例化表示）。
2. **内建表 schema**：名字 → 语义 + lowering 目标（含 mutating 语义、返回值、对齐）。
3. **runtime ABI**：函数签名、谁保存寄存器、字符串/数组的内存布局（我们的布局，不必与游戏一致，
   但必须与源码语义一致）。
4. **装配器接口**：输入（目标文件/镜像 + 符号表）→ 输出（blob 字节 + entry 偏移 + 生命周期句柄）。
5. **DLL 壳**：三模式 + 失败转交 + 记录格式（沿用 `manifest.tsv`）。
6. **差分宿主驱动协议**：假内存布局、命令写入格式、快照格式。
7. **构建与链接方式**：`LLVM-C.dll` + C API、三元组 `x86_64-pc-windows-gnu`、构建脚本直调 w64devkit g++
   （照抄 `tools/deploy_compile_hook.py`），**不引入 CMake**（只在改为从源码构建 LLVM 或走 MSVC 时才需要）。

**工作纪律**：一个写者一个目录（静态分区）；每次提交跑判据入口；未支持构造一律"显式报错 + 回退"，禁止静默降级。

**流程禁令（2026-10-17 定）**：

- **禁止「先在 Python 上做、再移植到 C++」**。产品代码（§3.1 的 C++ 部分）一律直接写成 C++17；
  语义不明时查语义档案（`docs/facts/semantics.md`）**当规格读**，不按「某个实现这样做」推断。
- IR 质量实验用手写 `.ll`（语言无关，M1 已验证）走 `cli/llvm_prog.cc`，不需要动前端，也不需要在 Python 上做。
- **M4.5 是对 M3 已接受原型的一次性收尾**（把已跑通的语义搬成 C++），它不构成“先 Python 后移植”的先例；
  此后不再有该模式。

**不使用 git worktree**（已定，2026-10-15）：agent 调用命令多用绝对路径，worktree 会让这些路径指向主工作树。

**并发写者协调**（已定，2026-10-15）：

1. 默认**静态分区**：派活时切分文件集合，同一时刻一个目录只有一个写者；读者不限。
2. 两把显式锁（放 `work/tmp/locks/`，不入 git）：
   - `game.lock` —— 一切游戏内实验（部署/更新转发层、simctl 驱动、采主频，以及任何动游戏目录或
     `work/hook`、`work/opt` 的操作）必须持有，同一时刻只有一个持有者；
   - `ledger.lock` —— `docs/facts.md` / `docs/status.md` 等共享账本。非主导者只写自己的
     `docs/notes/<agent>-<topic>.md`，由主导者折叠进账本。
3. 锁协议：一个锁 = 一个文件，内容含 owner（agent / run id）、获取时间、用途、预计路径集合；
   **一次性申请全部所需锁**（避免增量申请造成死锁）；拿不到就等或退避，不抢；带 owner 校验与 TTL，
   stale 锁由父级/收尾步骤清理；提交时只 `git add` 自己锁定的路径，提交后释放。
4. 锁只是**协作约定**，harness 不强制 → 它的价值全在“可见 + 文档化”，别指望它兜住不守规矩的 agent。

**回退语义（2026-10-18 定）**：

- **整单回退**：我们的实现失败 → 转交原版编译器，原样返回它的 `Result`（不自造错误消息）；
  `ours` 模式不转交，返回自造失败 Result。
- 回退原因按 §3.4 三轴记录；回退率 = 关闭回退的量化依据。
- 必须挡住崩溃：VEH 熔断已做；**超时**（`ours_timeout_ms` 默认 5000）；⚠️ 未测 LLVM `report_fatal_error`
  是否穿透壳 —— 全仓没有 `LLVMInstallFatalErrorHandler`。

---

## 9. 里程碑（按能力划分，不写工期）

- **M0 观测设施**：语料固化（`work/hook/all` 180 份 → 去重 98 份 + manifest + 构造打标签）+ 离线参考产物生成
  + 转发层可切换实现（`flags`/Result 记录已完成）。
- **M1 spike 最小闭环**：手写 LLVM IR → `-O2` → 我们的装配器 → 游戏里跑通一个空程序。
  验收点：**能取到 `LLVM-C` 的导入库（官方包只有 DLL，可能需要 `gendef`/`dlltool` 造）并用 g++ 链上、
  跑通 `LLVMRunPasses` 并拿到目标码字节**、DLL 形态、装配链路、bit62 置 1 后不被 free、
  entry 包装（手写 shim）、不崩游戏。
- **M1.5 收益探针 —— 2026-10-17 负责人决定终止**：宿主内直译慢 ~25%、热状态进寄存器快 ~2.95×；
  终止原因是**很难找到「性能占用高、又方便手动翻译」的电路**，游戏内无证据。数据见 `docs/notes/2026-10-17-m1.5-probe.md`。
- **M2 差分宿主可用**：参考产物自比对通过（宿主可靠性得证）。
- **M3 生成代码子集编译 + 逐 tick 状态等价**（RV32IM 电路）：宿主面达成（实现为已删除的 Python 原型），
  产物与参考 10 tick 全等。产品实现由 M4.5 补。
- **M4 主频对照**：转发层切 ours/reference，`watch_freq.py` 出对照结论。
- **M4.5 前端换 C++（2026-10-17 定，M5 的前置；已完成）**：`frontend/` + `lower/` 换成 C++17（范围只到生成代码子集）。
  判据：①逐 tick 等价 ②§7.5 的延迟门与依赖门 ③判据入口的生产者切到 C++ 路径后全绿。
  **M4.5 之后，产物一律来自 C++ 前端**（M4 期的预编译 blob 例外不再适用）。
- **M5.5 编译器重构（2026-10-18 定）**：按 §3.2–§3.4 分层 —— `sema` 从 `lower.cc` 拆出（体内无 `LLVM*`
  的 94 个函数/1545 行为下限）、`lower/` 改名 `codegen/`、消除 `FnState` 的 save/restore（10 字段/7 对）、
  统一错误三轴、实例化移到 sema 并做键规范化。
  判据：①重构前后同一语料的产物**逐字节相同**（重构不改语义）②§7.6 的接缝/最小用例/`assert` 落地。
- **M5 全量语料覆盖**：campaign tests + stdlib（路线 A），覆盖率矩阵达标。**必须建在 M4.5 的 C++ 前端上**。
- **M6 回退关闭**：语义档案完备，`ours` 可独立承担全部编译。

---

## 10. 版本锁定与 Steam 更新后的重验

| 项 | 当前值 |
|---|---|
| 游戏 | appid `1444480` / buildid `25294874` / `StateFlags 4` / 最后更新 2026-09-14 |
| 路径 | `F:\opt\SteamLibrary\steamapps\common\Turing Complete`（`D:\symlink\steamapps_c2` 为符号链接） |
| 参考编译器 | `compile_real.dll`（1.8 MB） |
| 转发层 | 游戏目录 `compile.dll` = `build/compile_hook.dll` |
| LLVM | 固定到项目启动时的最新稳定版 |
| 编译延迟预算 | 最终 5 s（调试期放开） |

**重验脚本（设计）**：把 T1–T5 的结论写成**按符号名定位**（`nm`/`find_sym` 查符号 → 反汇编该函数
→ 断言指令模式）的可重跑脚本，Steam 更新后先跑它，不要硬编码地址。断言清单：

1. `compile` 导出仍存在，且 exe 内只有 1 个调用点、`flags` 实参为 0；
2. `jit` 内存在 `memcpy(dest, ptr+8, len)` 与尾跳 `jmp`；
3. `handle_request_compile_and_run` 内存在 `out[3]` 的 `test/jne` 失败分支；
4. `eqdestroy` 内存在 `testb $0x40,0x7(p)` + `rawDealloc`（bit62 门控）；
5. 产物头 8 字节的值仍不被读取（除 bit62）。

---

## 11. 未确认项与开放决策

| # | 问题 | 影响 | 处理 |
|---|---|---|---|
| U1 | 失败时错误消息的 UI 显示点 | 只影响我们自造错误消息的收益 | 未追到；按"整单转交原版编译器"绕过 |
| U2 | exe 里 `Solution took too long to compile` 是编译超时还是仿真超时 | 若是编译超时，它才是 5 s 预算的真正上界 | 待查 |
| U3 | `print_using_stack` 在 Windows x86-64 是否真是空操作 | 决定 runtime 是否要复刻打印 | 待验证 |
| U4 | 语言版本（当前安装版 vs 社区提到的 TC 2.0 新版 Simplex） | 文法差异会改变前端 | 锁定当前构建；出现新版时重新对齐 |
| U5 | 内建清单完备性 | 决定 lowering 表 | 语料已扩到 180 份（去重 98）+ 与 `compile.dll` 的 `INTRINSIC_*` 常量表交叉验证 |
| U6 | 代码落库位置 | 影响 agent 工作流 | **已定：`simplexc/`**（2026-10-15） |
| U7 | benchmark 与性能目标 | 暂缓 | 测法已定（`watch_freq.py` + 转发层切换） |
| U8 | 主频/bench 层重写的架构（模块与接口） | 决定 A/B 验证基础设施 | **已定**：见 `docs/specs/2026-10-15-freq-measurement-design.md` |
| U9 | 并发写者协调 | 决定多 agent 并行纪律 | **已定：静态分区 + `game.lock` + `ledger.lock`**（见 §8） |
| U10 | 原版编译器调用 kernel32 的实际序列（Win64 ABI / 影子空间） | 决定 extern 调用能否直接由 LLVM 生成 | 反汇编核对 |
| U11 | 游戏/测试代码是否会接触我们 runtime 分配的内存 | 决定我们的 arena 布局能否自由 | 语料扫 `set_ram_value`/`get_ram_value`/`get_memory` 的实参 + 差分白名单验证 |
| U12 | 生成源码里 `#` 开头的编译期构造是否只有 `#if in_scope` | 决定预处理器范围 | M0 对 98 份语料 grep 普查 |

---

## 12. 证据索引（可复现）

| 事实 | 复现方式 |
|---|---|
| 导出表 3 个符号 / 调用契约 | 手写 PE 解析脚本；或 `objdump -p compile_real.dll` |
| Result 5 字段语义 / 成功失败路径 | `objdump -d --start-address=0x14021cf20 --stop-address=0x14021d310 "Turing Complete.exe"` |
| `flags=0` | `0x14021cff6 xor %r8d,%r8d`；或 `work/hook/manifest.tsv` 的 flags 列 |
| 游戏侧 JIT 流程 | `objdump -d --start-address=0x14021cc60 --stop-address=0x14021cd40` |
| `out_len` = section 大小、memcpy 长度 | `0x14021cc89 mov %ecx,0x20(%rsp)` / `r9d=0` / `0x14021cce9 memcpy(dest,out[1]+8,out[0])` |
| 入口寄存器状态（`rdi=entry` / `rsi=dest` / `rbx=Result*`） | 尾跳 `lea (%rsi,%rdi,1),%rax; jmp *%rax` |
| blob 头 bit62 门控 | `objdump -d --start-address=0x14001c260 --stop-address=0x14001c2a0` |
| entry 每次进入 1 次 / 程序模型 | T3 子代理报告（按符号名定位的调用链） |
| stdlib 前缀 27711 B（含 55 处 CRLF）/ 1503 行 | 从 `compile_real.dll` 偏移 `0x119F48` 提取；工作副本归一化后 27656 B |
| 内建清单 | 对前缀做「被调用但未定义」统计 |
| 指令质量统计 | `objdump -D -b binary -m i386:x86-64` + 计数脚本 |
| 编译延迟 | 离线编译 419888 B 源码计时（≈2.09 s） |
| 语料规模 180 份 / 去重 98 份 | `ls work/hook/all/*.si \| xargs md5sum \| awk '{print $1}' \| sort -u \| wc -l` |
| 180 组「源码, 产物」配对（**无 entry**） | `ls work/hook/all/*.si \| wc -l` 与 `*.bin` 同名配对；`work/hook/manifest.tsv` 只有最近一次会话的 9 行，`all/` 里无 manifest |
| 参考产物 bit62 = 0 | 读 `work/hook/all/hook_out_*.bin` 头 8 字节：低 32 位 == 文件长度、bit62 为 0 |
| 游戏版本 | `F:\opt\SteamLibrary\steamapps\appmanifest_1444480.acf` |
| 社区前作 | `github.com/michalusio/TC-si`（MIT） |

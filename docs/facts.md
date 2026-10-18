# 事实

被反复引用的结论、契约、机制、判据。**就地改写，不留旧值**（旧值看 git 历史）。
每条必须带「验证：」与「最后验证：」。验证方式写「静态核对」的，表示读代码/反汇编对过；
写命令的，表示那条命令跑通。

## compile.dll 转发层

### compile() 的调用契约
签名 `Result* compile(Result* out /*rcx*/, NimStr* src /*rdx*/, int flags /*r8d*/)`；
40 字节输出 = `{out_len, out_ptr, entry_off, err_len, err_ptr}`：`out_len` 是缓冲总长（含 8 字节头）、
`out_ptr` 指向缓冲、`entry_off` 是产物内锚点偏移（相对机器码起点），`out_len == 0` 即编译失败。
`err_len/err_ptr` 是**错误消息字符串** `{len, ptr}`：成功时两者都是 0，非 0 即失败。
`flags` 实测恒为 0（游戏侧唯一调用点 `0x14021cff6 xor %r8d,%r8d`；manifest 10/10）。
验证：`objdump -d compile_real.dll`（compile @ `0x2fddd5fd0`）+ exe `0x14021cf20` 段
      + `work/hook/manifest.tsv` 实测
最后验证：2026-10-15

### 失败时不能只把 out_len 置 0
`out_len == 0` 且 `err_len == 0` 时游戏走**成功路径**（exe `0x14021d08c` → `createThread`），
`jit` 里 `len <= 0` 触发 `raiseIndexError2`。**失败必须给非 0 的 `err_len`**
（判定点 exe `0x14021d07b`/`0x14021d083`：`test %rbx,%rbx; jne 0x14021d198`，失败分支跳过
`send_command(1)` 与 `createThread`，不跑新代码）。
验证：反汇编 exe `0x14021cf20`–`0x14021d310`
最后验证：2026-10-15

### 游戏如何执行产物
`jit__modelZsimulationZjitZjit_u1807`（exe `0x14021cc60`）：分配 `out_len` 字节可执行内存
（dynlib 取到的 CreateFileMappingW(PAGE_EXECUTE_READWRITE) / MapViewOfFileEx，🔍 判读）
→ `memcpy(dest, out_ptr + 8, out_len)` → `c_jit()`（exe `0x140001a40` 实测就是 `ret`）
→ `jmp *dest + entry_off`。**不设置任何寄存器、无参数、无返回值** →
产物内部调用约定由编译器自定，entry 自己保存现场。
⚠️ memcpy 读到 `out_ptr + 8 + out_len`，比缓冲末尾多 8 字节（参考实现靠 Nim 字符串余量兜住；
自研实现要显式留 8 字节 padding）。
验证：反汇编 exe `0x14021cc60`–`0x14021cd40`、`0x140001a40`
最后验证：2026-10-15

### 产物头 8 字节：只有 bit62 被读
头 word 的**值**游戏从不读（全 exe 只有 `jit` 的 `memcpy(ptr+8, len)` 一处读产物）；
但 `eqdestroy___pureZtimes_u2668`（exe `0x14001c260`）会 `testb $0x40,0x7(p)` 看头 word 的 **bit 62**：
置位 → 不 free，否则 `rawDealloc(allocator__system_u7164, p)`。错误消息指针走同一门控
（成功路径 `0x14021d127` → `deallocShared`）。
→ 自研实现的产物头**必须置 bit62**，否则游戏会用自己的分配器 free 我们的指针。
代价：`analyze_jit_code.py` 的「头值 == 文件总长」自检要掩掉这一位。
验证：反汇编 exe `0x14001c260`–`0x14001c2a0`、`0x14021d11a`–`0x14021d130`
最后验证：2026-10-15

### 产物的内存所有权与释放时机
游戏只在 bit62 未置位时释放产物，**无 refcount**（唯一性靠 `wasMoved`/move 保证）。
`jit_function` 在调 `jit` 前会拷贝一次产物字符串（`eqcopy` @`0x14021cd78`），
且 `handle_request_compile_and_run` 每条请求都先 `send_command(3)+joinThreads` 再 `compile()`，
而 `compile()` 全 exe 只有一个调用点、单线程串行
→ 「下次 compile 开始时释放上一份产物」安全。
验证：反汇编 exe `0x14021cf20` 段（`0x14021d230` 起的 join 路径）
最后验证：2026-10-15

### entry = 顶层语句块，每次编译只进入 1 次
程序根是**顶层语句块**：空源码 / 只定义函数 → 产物恒 8581 字节（函数被 DCE）；
顶层写一句 `store(...)` → 产物 +13 字节且立即数落在 `0x215c`。顶层块末尾有隐式 `thread_exit()`。
`jit` 尾跳进 entry 后无返回路径，`jit_function` 全 exe 只作为 `createThread` 的线程过程出现一次，
`handle_request_compile_and_run` 每次请求都无条件编译 + 起线程
→ **每次编译 entry 恰好进入 1 次**，同一产物不会被重复进入；每次进入都重新初始化
（产物内数据每次被 memcpy 覆盖）。
验证：离线探针（空源码 / 仅定义函数 / 顶层 store 三例对比）+ 反汇编 exe `jit`/`jit_function`
最后验证：2026-10-15

### 壳的失败路径与熔断（三模式 + per-call `impl` 行）
- ours 失败不再早返回：写 `error` 行（`stage=ours kind=<§8 分类串>`）+ `impl` 行 + manifest 行（`impl=ours_failed`）。
- ours 段超时预算：`TC_OURS_TIMEOUT_MS` > `opt.ini [opt] ours_timeout_ms` > 5000 ms；超预算丢产物、按模式失败/回退，
  与只管改写子进程的 `OPT_TIMEOUT` 无关。
- VEH 只包 ours 段（`g_in_ours` 卡作用域）：命中 → `error stage=ours_fault`、本进程永久熔断（之后全走 reference）、
  按模式返回。恢复原语 = `RtlCaptureContext` + 把上下文搬回恢复点 + `EXCEPTION_CONTINUE_EXECUTION`；
  窗口外的异常照常穿透（独立进程验过；游戏进程内未验）。
- `impl` 行 per-call（含 reference）：`impl=<reference|ours|fallback|ours_failed> mode=<配置值> result=<ok|reference|fail>`。
验证：`python simplexc/tools/hook_offline.py`（8 个 case 全过）；原始证据 `work/tmp/hook_test/evidence/*`
最后验证：2026-10-17

### compile() 的内部流程
`rawNewString(len + 27711)` → 拷 27711 字节固定前缀 → 拷用户源码 → `compile_source__OOZcore_u145`。
验证：静态核对（`objdump -s` 27711 字节前缀）
最后验证：2026-10-11

### compile() 可脱离游戏进程调用，产物**同一 boot 内**逐字节可复现
两个独立进程编译同一份源码，产物 md5 相同（**同一 boot 内**；跨 boot 不成立 —— 参考产物里烘焙着
`compile_real.dll` 的加载基址，见 §「产物的绝对地址绑定 compile_real.dll 的加载基址」）。
「离线重编译 == 游戏内捕获产物」这条只在**归一化掉 DLL 基址差**之后成立：98 份语料归一化后逐字节相同，
差值恒为 `0x380000`，未分类差异 0 处。
历史例子（同一 boot 会话内）：`a18_run1.bin` = `a18_run2.bin` = `hook_out_002.bin` = `002.raw.bin`
（md5 `12cb7184…`，204623 B = 8 字节头 + 204615 机器码）。
验证：`python simplexc/tools/recompile_corpus.py --check`（退出码 0）+ `corpus/manifest.tsv` 的
      `ref_match` / `ref_match_rebased` / `rebased_delta` 列；旧例：`python tools/try_compile_dll.py --compile --src <样本>` 跑两次比 md5
最后验证：2026-10-16

### 游戏内 JIT 代码与离线产物逐字节相同
故映射可直接用于采样地址。19 份产物 md5 全等，`locate()` 对 19 个内存候选做整段比对全 pass。
验证：跑 `tools/profile_by_image.py`，看 locate 的整段校验结果
最后验证：2026-10-11

### 产物文件前 8 字节是长度头
不是机器码；头字段的值**等于文件总长（含头自己）**，不是数据长度。
（自研实现置 bit62 后，头值 = 总长 \| bit62，见「产物头 8 字节：只有 bit62 被读」。）
验证：`stat -c %s` 与 `od -An -tu8 -N8` 比对任一 `work/hook/hook_out_*.bin`
最后验证：2026-10-11

### 转发层与真编译器
部署期间游戏目录的 `compile.dll` **是转发层本身**，与 `build/compile_hook.dll` 逐字节同源；
真编译器是 `compile_real.dll`。查静态信息（符号 / 导入表 / 反汇编）一律认后者。
验证：`md5sum build/compile_hook.dll "<游戏目录>/compile.dll"` 应相同
最后验证：2026-10-11

### 编号机制
编号计数器是 DLL 全局变量，**每次加载（游戏启动）从 1 递增**；「同号即一对」——
一次 `compile()` 取一个号，`hook_src_005.si` 与 `hook_out_005.bin` 是同一次调用。
**跨会话判断新旧要看 mtime**，不能只按编号取最大。
验证：静态核对（`compile_hook.c`）
最后验证：2026-10-11

### 清空语义
`work/hook/` 与 `work/opt/` 同为转发层工作区、编号同出 `g_seq`，**每次加载一起清空**
（各清哪些扩展名见 `compile_hook.c` 的 `purge_captures` / `purge_workdir`）；
`work/freq/` 是采样数据，跨会话保留。
验证：静态核对（`compile_hook.c`）+ 实测（连续两次加载，`manifest.tsv` 都从 001 重新开始）
最后验证：2026-10-13

### 产物 rewrite 模式的权威来源
只认 `hook.log` 的 `opt` 行（为何不能读 `opt.ini`，见 `watch_freq.py` 的 `opt_info`）。
验证：静态核对（`watch_freq.py`）+ 历史铁证（`hook_out_005`/`006` 产物 sha 相同却标了不同模式）
最后验证：2026-10-13

## 参考实现 compile_real.dll（编译器 / 产物侧）
### 产物的绝对地址绑定 compile_real.dll 的加载基址
参考产物里的 `movabs r64,imm64` 全是 image 空间地址，落在 **compile_real.dll 自己的镜像**里：
会话里 DLL 加载在 `0x7FF9A0DE0000`，产物里的 `0x7FF9A0DE1310` = DLL 内 RVA `0x1310`，字节是
`48 FF 25 59 70 14 00` = `jmp QWORD PTR [rip+0x147059]`（目标 RVA `0x14836F`，在 `.idata`）→ **导入 thunk**；
另有一批指向 DLL `.bss` 槽位（如 base + `0x131280`）。语义上仍是调 kernel32，但**绑定的是 DLL 的加载基址**。
→ **产物跨 boot 不能逐字节相等**（基址随 boot 变、同一 boot 内跨进程恒定）；离线重编译只能做到
「两边都归一化到 DLL 镜像偏移后逐字节相同」。
验证：`python tools/classify_abs_addrs.py`（VirtualQuery/GetMappedFileNameW）、
      `objdump -D -b binary -m i386:x86-64 -M intel --start-address=0x910 compile_real.dll`、
      模块基址枚举（反例：本 boot 的 `KERNEL32.DLL` 在 `0x7FFA…` 段，产物里的 `0x7FF9…` 不在其中）
最后验证：2026-10-16

### 数组/字符串布局与 Nim seq 同构
`Arr[T]` 的载荷指针 `p`：`p[-8] = (len << 16) | cap_log2`（len 是 48 位，低 16 位是 log2 容量），
`len = u64(p-8) >> 16`，容量字节 = `(1 << u16(p-2)) >> 1`；空数组用 `empty_array_address()` 哨兵。
必须同构的理由：游戏把自己的 seq/字符串按硬编码绝对地址交给生成代码
（`raw_cast(<Arr[U64]>, 2416573480960)`、`#SIMULATION_STATE`）。
验证：读 stdlib 前缀的 `allocate_raw` / `get_length` / `set_length` / `get_capacity`
最后验证：2026-10-16

### 内建/宏名表就在 DLL 的 .rdata 字面量池里
`INTRINSIC_{STANDARD,UNARY,BINARY,BINARY_MUTATING}_NAMES` 是 DLL 里的全局（符号与地址见
`recovered/compiler-src/OO/types.nim.stub:714-717,769-772`），名字本身作为 Nim 字符串字面量按**声明顺序**
紧邻排列在 `.rdata`；`mac_*` 组共 17 个（`mac_load`/`mac_store`/`mac_raw_cast`/`mac_size`/…）。
→ 内建清单（主设计 §11 U5）的权威来源是这里，不是从前缀反推。
验证：`python tools/extract_nim_strings.py <dll> lits --out FILE`，看文件偏移 `0x1104A0`–`0x1106A0` 段
最后验证：2026-10-16

## 语料与判据

### 判据入口：一条命令跑完现有判据
`python simplexc/tools/check.py`（`--list` / `--only <name>` / `--json <path>`）= `corpus`（manifest 三项断言）
+ `host_selftest`（M2 自比对）+ `hook_offline`（壳的 8 个 case）+ `probe_006`（M1.5，可选）。
当前 4 PASS / 0 FAIL / 0 SKIP；一步失败不影响其它步，JSON 里每步有 rc/秒数/摘要。
验证：跑它看每步 rc；故意让一步失败时该步 rc≠0 且 JSON 如实记录
最后验证：2026-10-17

### 生成源码里的游戏内存地址按「游戏进程实例」变
`commands` / `settings` / `#SIMULATION_STATE` 等绝对地址写在生成源码里，同一台机器上**每个游戏进程实例一套**
（实测：同一进程内 NAND 关卡与元件工坊的源码都是 `2270676779008` 段；12:24 那次会话的捕获是 `2416573349888` 段）。
→ 含地址的产物（我们的 blob、参考产物）**只在生成它的那个进程里有效**；游戏内验证必须（违反这条的实测后果见下一条）
「同一次会话内：抓源码 → 生成产物 → 部署」。宿主不受影响（它自己按源码里的地址 VirtualAlloc 假内存）。
验证：`grep -m1 "var commands" work/hook/hook_src_{003,005,040}.si` vs `simplexc/corpus/src/006.si`
最后验证：2026-10-17

### clang -O2 来源的产物在游戏进程里跑到过协议主函数（M1 缺口已闭合）
2026-10-17 16:49:53（`manifest.tsv` seq 007）：`impl=ours`、`out_len=0x0B54`（2900 B 标记版）、`r2=0x7A0`；
壳记 `impl=ours mode=ours result=ok guard=veh`；产物自己写的 `work/tmp/equiv/equiv_cc_and.log` 出现两行
`equiv_cc_and tc_main, sim_state=2270726717440`（= `0x210B1DC0000`，与该会话 `addrs.h` 里 `A_SIM_STATE` 逐字相同）。
→ 手写的 LLVM 产物确实被游戏 JIT 执行了，并跑进了协议主函数。标记走的是 kernel32 跳板（与游戏内存无关），
所以它能在「后面写游戏内存时崩掉」的情况下依然留下证据。
验证：`awk -F'\t' '$1=="007"' work/hook/manifest.tsv`；`grep 007 work/hook/hook.log（迁移前的记录在 work/opt/hook.log）`；`cat work/tmp/equiv/equiv_cc_and.log`
最后验证：2026-10-17

### 跨游戏进程复用含地址的产物 = 写未映射地址，崩在 JIT 代码里
2026-10-17 16:50 实测：用 16:39（进程 A，`0x210A…` 段）生成的产物，喂给 16:49 的进程 B（`0x20C4…` 段，
16:47 崩过一次后重启）。我们的代码正常进了 `tc_main`（标记文件两行），随后按旧地址写游戏内存 →
`0xc0000005`；WER 记 `Faulting module: unknown`、异常偏移 `0x20C57020197`（JIT 区，`MapViewOfFileEx`
基址 64 KB 对齐 → 对应产物内偏移 `0x197`），CrashDump 落 `%LOCALAPPDATA%\CrashDumps\Turing Complete.exe.51276.dmp`。
壳**不会**记 `error stage=ours_fault` —— VEH 只包 `compile()` 那一段，JIT 执行期的异常不归它管。
验证：`%LOCALAPPDATA%\CrashDumps` 里 16:47 / 16:50 两份 dump + `wevtutil` 的 Application 日志（Id 1000）
最后验证：2026-10-17

## M3 语义档案（实验后定死；没测的别当已定）

### U-L1：`load`/`store` 的 `p` 是**字节地址**，无对齐要求，窄类型写只写该宽度
- `p + 1` = 下一字节（不是下一个元素）；`p + Int k` → lowering 里直接 `add` `k`，**不乘元素宽度**。
- 非 8 对齐的 `U64` 读/写都**静默成功**：精确落在「该地址起的 8 个字节」，**不向下对齐**、**不越写邻居**、不 AV。
- `store(p, U8 v)` 只写 1 字节，邻居保留（不是提升成 8 字节写）。
- → lowering：`load`/`store` 的 `align` 只能声明 **1**，且**不得插入对齐检查/陷阱**（参考实现没有这个假设）。
- 未测：`U32/U16/U1/U5/U3` 的读写宽度（属 U-L7）、`Arr[i]`/`Str[i]` 的单位（属 U-L2）、跨页边界的非对齐读写。
验证：`simplexc/dossier/ul1_load_store.md`（5 个片段 × 编译 + 宿主分页转储，片段正文抄在文件里，含三条命令）
      复跑观测：`PYTHONIOENCODING=utf-8 python simplexc/tools/host/host.py --src work/tmp/ul1/q2_store_unaligned.si --bin work/tmp/ul1/q2_store_unaligned.bin --ticks 1 --entry 0x212F --out <x>.jsonl --dump-pages <d>` → 页内字节 `aa 88 77 66 55 44 33 22 11 aa…`
最后验证：2026-10-17

### U-L7：位宽是**真语义** —— 转换与运算结果都按 `(1<<N)-1` 掩码
- `U8 (250 + 10)` = `0x04`；`U1 (1 + 1)` = `0`；运行期值同样（`U8 (3000)` = `0xB8`、`U5 (33)` = `1`）。
- 截断发生在**运算结果类型**上：`U5 31 + U5 1` = `0`，而 `U64 (U5 31) + U64 1` = `32`。
- 非 2 的幂宽度**全部掩码**，含 33 位：`U33 0xFFFFFFFFF` → `0x1FFFFFFFF`，`2^33` → `0`。
- 提升分两路（**不是一条规则**）：
  · **编译期常量**按**源类型的符号性**：`U64 (S32 -1)`、`S64 (S8 -1)` 符号扩展；`U64 (U32 0xFFFFFFFF)`、
    `S64 (U8 0xFF)` 零扩展。
  · **运行期窄值**（变量/`load`/运行期表达式）一律 `zext(trunc_W(v))`，**与符号性无关**：
    `S32` 变量 −8 → `U64` 得 `0x00000000FFFFFFF8`（不是 `0xFFFF…F8`）。
    排除过「读 8 字节、高 4 字节残留」：把槽预填 `0xFF` 与 `0x00`，整槽内容不同
    （`FFFF…F8` vs `0000…FFF8`）但转换结果**逐位相同**。
- `store(p, v)` 写入宽度 = `v` 类型的字节宽度（预填 `0xFF` 后 `store(p, U32 …)` 只覆盖 4 字节）。
- 窄读掩码：`0xFF` 读成 `U5`→`0x1F`、`U3`→`0x07`、`U7`→`0x7F`、`U1`→`0x01`、`U8`→`0xFF`。
- → lowering：**不要**用 LLVM 原生 `iN`（N∉{8,16,32,64}）承载这些宽度（后端对非标准宽度的假设不可控），
  用「按 64/32 位算 + 显式掩码」，掩码点就两处：**转换**与**运算结果**。
验证：`simplexc/dossier/ul7_promote_truncate.md`（7 片段 × 3 tick，`evidence.txt` 21 行 ok，字节证据在 `bytes.txt`）
最后验证：2026-10-17

### U-L8：`>>` 是「扩到 64 位后逻辑右移再截断」，移位量按 W 取模，除零不 trap
- `x >> n` = `trunc_W( lshr( ext64(x), n & (W-1) ) )`。**永远是 lshr，不是 ashr**；
  `ext64` 的扩展方式与 U-L7 同一条规则：**常量按声明类型的符号性**，**运行期一律 `zext(trunc_W)`**。
  所以常量 `S32 -8 >> 1` = **-4**（sext 到 64 → lshr → 截回 32 得 `0xFFFFFFFC`），而常量
  `S64 -8 >> 1` = `0x7FFF…FFFC`（已在 64 位，lshr 不截断）；运行期窄值右移则先 zext 再 lshr
  （`S32` 变量 −8 右移 ≠ −4，见 U-L7 那条的运行期规则）。
- 移位量按位宽取模：`U32 1 << 32` = 1、`1 << 33` = 2、`1 >> 32` = 1；**不是**「≥ 位宽就 0」。
  `U8 1 << 8` = 1（`8 & 7 = 0`，结果再截断到 U8）。
- 除零**不 trap**：商 0、余数 = 被除数；无符号/有符号/常量/运行期一致，`exited_clean`、3 tick ok。
- `asr(x, n)` ≠ `x >> n`：W<64 且 n<W 时相同；n ≥ W 时 `asr` **饱和成符号填充**而 `>>` 回绕；
  **W=64 时根本不同**（`asr` 算术 −4，`>>` 逻辑 `0x7FFF…FFFC`）。
- 未测：`asr` 在 W=16 / W>64 的饱和；见下一条的 `/`、`%`。
验证：`simplexc/dossier/ul8_shift_div.md`（24 片段 × 3 tick，72 行 ok，逐条命令在文件里）
最后验证：2026-10-17

### `/`、`%`：opcode 由**被除数的声明类型**决定，除数完全不参与
- `S8/S16/S32/S64/Int` 被除数 → `sdiv`/`srem`；`U8/U16/U32/U64` 被除数 → `udiv`/`urem`。
  **除数**的类型、常量性都不参与（两个方向都实测）。改被除数类型会翻（同为位模式 `0xFFFFFFF9`：
  `S32` → −3，`U32` → `9223372036854775804`）。
- ⚠️ 参考实现自身有**一处不自洽**：由两个**窄类型字面量折叠**出来的常量，被当成「W 位模式零扩展成 64 位」
  再送进**有符号**除法 —— 所以 `(S32 0 - S32 7) / (运行期 2)` = `sdiv(4294967289, 2)` = **2147483644**，
  而 `(S32 -7) / (运行期 2)` = **−3**。同一个值 −7、写法不同、结果不同。
  → 这不是语言语义，是参考实现的实现细节；要保真就得复刻，**不要「修正」它**。
- 二维表（`/`，顺序 = 常量/常量、运行期/常量、常量/运行期、运行期/运行期）：`−3, −3, 2147483644, −3`；
  `%` 对应 `−1, −1, 1, −1`。
- 两个操作数**都是**常量 → 编译期折叠：用精确值 + 被除数符号性，结果**截断到 W**
  （`S8 0 - S8 7` ÷ `S8 2` 折出 `0xFD`=253）；**运行期**结果**不截断到 W**（`f8` 的 `+280` 是 64 位结果 −4294967289）。
- 宽度：S8/S16/S32 符号性一致；「折叠窄常量零扩展」那个坑三档都在（`S8 0 - S8 7` ÷ 运行期 2 = 124、`S16` = 32764）。
- 未定：零扩展的内部机制；U32 运行期值的 udiv 用 64 位表示（未见截到 W）；运行期×运行期的溢出没测。
验证：`simplexc/dossier/div_sign.md`（8 片段 × 2 遍，`check_claims.py` 断言报告里 100 个数字、0 失败）
最后验证：2026-10-17

### LLVM 目标码必须用 `RelocMode=PIC`（`Static` 与「拒绝内部绝对重定位」互斥）
产物要被整体 `memcpy` 到编译期未知的 JIT 基址执行 → 位置无关。实测 `RelocMode=Static` 下 LLVM 对
「往内部全局写常量」生成 `movq $imm32,[disp32]` 绝对寻址，产物里出现 6 处 `IMAGE_REL_AMD64_ADDR32`
（指向内部全局 `cycle`/`last_command_id`）→ 被装配器按契约 §4-2 拒（`status=3`）；
改 PIC 后 `.text` 只剩 `REL32`（+ `.pdata` 的 `ADDR32NB`，整节丢弃）→ 装配通过。
旁证：clang 对该三元组默认 `-mrelocation-model pic`（所以手写 C 链路从没踩到）。
验证：`docs/specs/2026-10-15-simplexc-interfaces.md` §7 附注；`simplexc/lower/ir_build.py` 的 RelocPIC
      + `docs/notes/2026-10-18-lowering-v0.md`
最后验证：2026-10-17

### M3 首个里程碑：`.si` → 我们的 lowering → 产物，006 与参考产物**逐 tick 全等**
`simplexc/lower/ir_build.py`（AST → LLVM-C → 目标码 → `tcblob`）吃 `simplexc/corpus/src/006.si`：
产物 3508 B、entry `0xA90`；宿主 10 tick 与参考产物（entry `0x212F`）**差异 0 条**
（页哈希 + `sim_cycle` + extern 序列全同）。**整条链没有手写 C 参与**：
`decls` → `expr` → `ir_build` → LLVM `default<O2>` → `tcblob`。
静态：我们的 708 条指令 vs 手写 C 版 508 条（差在 `alloca` + `align 1` + 全 i64），参考产物 3090 条。
验证：`python simplexc/lower/ir_build.py --src simplexc/corpus/src/006.si --out <bin>`，再 `host.py`
      两边各跑 `--ticks 10` 比 `pages`/`sim_cycle`/`extern`（复跑命令见 notes）
最后验证：2026-10-17

### rv32im 的 lowering 缺口只有**两类**根因（250 条里 208 条是连锁）
`python simplexc/lower/ir_build.py --src simplexc/corpus/rv32im/rv32im.si --list-unsupported`
→ 250 条 / 250 行（约占 M3.0 规模 5181 行的 4.8%）：**38 条三目** `cond ? a : b`（前端已解析、
lowering 未做）+ **2 条空 struct**（`type Input Struct { }`，U-L4 布局未定）+ 2 条顶层
`var level_input = Input {}` + **208 条连锁**（`let x = cond ? …` 失败 → 后续名字未定义）。
→ 补掉三目与空 struct，rv32im 就能装配起来；其余构造（`load`/`store`/`asr`/`min`/`max`/`Arr[i]`/
`Str.len`/`memory_copy`/`Ptr` 算术/非 2 的幂掩码）都没再报 unsupported。
验证：`python simplexc/lower/ir_build.py --src simplexc/corpus/rv32im/rv32im.si --list-unsupported`
最后验证：2026-10-17

### M3 达成（宿主面）：rv32im 的 ours vs 参考产物 10 tick 逐 tick 全等
`python simplexc/lower/ir_build.py --src simplexc/corpus/rv32im/rv32im.si --out <bin>` →
**58612 B / entry `0xE1F0`**（参考 219088 B / `0x215B`，**我们 1/3.7**）；宿主 10 tick 差异 **0 条**
（页哈希 + `sim_cycle` + extern 序列）。`--list-unsupported` 从 250 降到 **0**：补了
三目 `cond ? a : b`（`condbr` + 两臂各自求值 + `phi`，保证只求值一支）与空 struct（`packed { i8 }` 占位）。
006 基线**逐字节不变**（3508 B / entry `0xA90` / md5 `9d888a41…`）。
⚠️ **成立边界**（别读成「完全等价」）：① 判据只比「相对 pre-run 基线**变过的页**」，rv32im 每 tick 只变 3 页
（协议 2 + `#SIMULATION_STATE` 第 1 页），`#DATA_*` 的 128 MB RAM 镜像**没进比对**；
② 游戏内真环境（真 Sleep/时钟、游戏设的 target_cycle）未验。
验证：`python simplexc/lower/ir_build.py --src simplexc/corpus/rv32im/rv32im.si --out <bin>`，再 `host.py`
      两边各跑 `--ticks 10`（ref entry `0x215B` / ours 看自己的 `.entry`）比 pages/sim_cycle/extern
最后验证：2026-10-17

### 宿主里那 272 MB RAM 镜像是**全零** —— 「逐 tick 全等」的强度边界
`#DATA_*` / `#RESET_DATA_*`（rv32im：128 MB×2 + 8 MB×2 等，合计 272.25 MB）在宿主里**从没被灌过内容**：
`regions` 段哈希实测等于「同长度全零缓冲的 sha256」。所以 rv32im 的「逐 tick 全等」只证明
**两边写出的内容一样**，**不证明复现了游戏里的 RAM 镜像**（游戏里那块是 ROM/程序映像，非零）。
→ 要真正演练这块，得给 `#RESET_DATA_*` 灌确定性图案（两边同图案）再跑 —— 宿主行为改动，未做。
验证：`simplexc/tools/host/host.py` 的 `_region_hashes`；`work/tmp/mirror/**`；两份 rv32im 快照的
      `regions` 字段逐段相同（6 段 / 10 tick）
最后验证：2026-10-17

### `memory_copy(src, dst, n)`：第一参数是**源**
我们的 lowering 曾把 `GetParam(0)` 当 dst —— 调用点传的是 `(源, 目的, n)`（源码
`memory_copy(#RESET_DATA_x - 2048, #DATA_x - 2048, N)`），于是 272 MB 镜像被**反向**拷贝并销毁。
这个 bug 在 RAM 全零时**完全看不见**（两边都读零）；灌确定性图案后第一 tick 就炸：
参考侧把图案拷进 `#DATA_*` 且 `#RESET_DATA_*` 完好，我们的两侧全零。
修法：`_body_memory_copy` 里 `GetParam(0)`→src、`GetParam(1)`→dst（`simplexc/lower/ir_build.py`）。
验证：`--seed-reset-data seq` 下 `work/tmp/fixmc/seq_{ref,ours}.jsonl` 的 `regions` 段哈希
最后验证：2026-10-17

### M3 强判据：灌图案后 rv32im 逐 tick 全等（含 272 MB 镜像真的被读写）
`--seed-reset-data seq|xorshift`（两边同图案；灌 `#RESET_DATA_*` 的 4 块 / 136.02 MB）
→ rv32im ours vs 参考产物 10 tick **差异 0 条**，6 段哈希逐段相同且**非零**
（DATA 段 `6165fb5d…` / `3e6dae7f…`；默认不灌时是 `cbcb79a5…` = 全零哈希）。
这条比不灌图案强得多：RAM 镜像被真写、真拷、真读，CPU 行为不再被简化。
验证：`selftest.py --src simplexc/corpus/rv32im/rv32im.si --bin <ours> --entry <e> --ticks 10 --seed-reset-data seq`
      + 我复跑的 `work/tmp/fixmc/{seq,xorshift}_{ref,ours}.jsonl`
最后验证：2026-10-17

### M4 结果（**Python 原型的 -O0 产物**）：游戏内 RV32IM 主频 —— ours **25.8 MHz** vs reference **5.37 MHz**（约 **4.8×**）
⚠️ 这里的 ours 是 Python 原型的 **-O0 机器码**成绩（`ir_build.py` 把 `CodeGenOptLevel` 写成 0）。
数字仍然真实，但**别拿它当产品基线** —— 产品实现的同题成绩见下一条（40.375 MHz）。
**判据入口一条命令可复现**：
`python tools/bench_rewrite.py --variant reference:off --variant ours:off --seconds 40 --json <out>.json`
→ `reference #009 5.369 MHz`、`ours #010 25.835 MHz`、**+381.2%**（各 39 样本；`opt.ini` 自动恢复、游戏不崩）。
手动的两轮（`simctl recompile` → `run` → `watch_freq --seconds 40`）给出同样的比值：
24.843/24.818 vs 5.204/5.169（4.78×）。产物：ours 58612 B / entry `0xE1F0`；reference 219088 B / entry `0x215B`。
reference 的 5.3–5.4 MHz 与设计 §2.5 的「改写前 5.4 MHz」吻合 → 靶子与量法没跑偏。
**中位数对切换过渡点不敏感**（drop 0/1/3/5：reference 5.369/5.372/5.369/5.368；ours 25.835/25.856/25.888/25.900），
而 mean 被第一个过渡点（3.30 MHz）拉低（5.309 vs 5.361）—— 这就是设计 §7 决策 3 选 median 的实测理由。
验证：`work/tmp/m4/ab3.json`（`schema=1`，含 raw samples）；`work/freq/bench_*.tsv`
最后验证：2026-10-18

### M4.5 游戏内验收：产品实现（C++ 前端 + 进程内编译）ours **40.375 MHz** vs reference **5.364 MHz**（**7.53×**）
同一次游戏会话内一条命令复现：`python tools/bench_rewrite.py --variant reference:off --variant ours:off --json <out>.json`
→ `reference #006 5.364 MHz`（7 样本）、`ours #007 40.375 MHz`（7 样本）。`hook.log` 原文：
`007  impl=ours  mode=ours  result=ok  budget_ms=5000  guard=veh` —— 无 error 行、无 `ours_failed`/`fallback`，游戏同 PID 存活。
同一份源码（sha `2461414c…`）两个实现的产物：ours **26404 B** / entry `0x63D0`；reference **219088 B** / entry `0x215B`。
相对 M4 的 -O0 产物（58612 B / 25.835 MHz）：体积小 2.22 倍、主频 +56.3%（7.53× vs 4.81×）。
**这条只证「快」，不证「对」** —— 正确性由宿主面的逐 tick 等价承担（`check.py` 的 lower_equiv 步）。
验证：`work/freq/m45_shell.json`、`work/freq/bench_20261018_133133.tsv`、`docs/notes/2026-10-18-m45-game-bench.md`
最后验证：2026-10-18

### campaign 的 `test.si` 是**测试片段**，不是完整编译单元
游戏自带的 `campaign/<关卡>/test.si` 只含 `const` + `def get_input` / `def check_output` + 顶层初始化代码，
**没有程序根**：`grep -lE "^label " simplexc/corpus/campaign/*/test.si | wc -l` = **0**；
而 hook 抓的生成源码 **98/98** 都含 `run_sim: while true { … }`。直接喂我们的 CLI 必然报「没有顶层语句块（程序根）」。
所以「campaign 92 份全装不出来」首先说明的是**语料形状**，不能读成 92 个编译缺口。
**游戏自己会把它们拼进完整程序**再交给 `compile.dll`：`def get_input` / `def check_output`（正是 `test.si` 的内容）
在 98 份捕获里 **98/98** 命中，`#CORRECT_OUTPUT` 命中 13 份且**值对得上**（`[U1 1,1]`、`[U1 1,0,0,0]` …）——
拿 `test.si` 当编译单元属于输入口径错误。
验证：`docs/notes/2026-10-18-campaign-survey.md`；`python simplexc/tools/m5/coverage.py --dir simplexc/corpus/campaign --no-diff`
最后验证：2026-10-18

### stdlib 前缀单独编不了：它没有程序根
`simplexc/corpus/stdlib_prefix.si` 里**没有顶层语句块**（没有 label），所以单独喂 CLI 必然报「没有顶层语句块（程序根）」——
它的正确输入形态是「**前缀 + 用户源码**整体」（参考实现每次 `compile()` 就是这么拼的）。
它是前端的**两大块**（按单元算 121 个顶层单元：82 def + 39 个算子单元），其中：`$` 可变参数 23 处 / 13 单元 ·
`asm` 块 5 单元 · `kernel32.'Name'(…)` 外部调用 8 处 · `[T] expr` 方括号转换 5 单元 ·
7 个内建/宏缺失（`array` 38 处，其余六个同样零命中）· 16 个单元签名带 `@Type`/`@Any`/`@Size`。
验证：`docs/notes/2026-10-18-stdlib-prefix-survey.md`；`./build/simplexc_cli.exe --src simplexc/corpus/stdlib_prefix.si --out <blob>`
→ 先死在更早的一层（词法 `$`，第 172 行）
最后验证：2026-10-18

### U-L14：`#if in_scope(name)` 的可见性规则（2026-10-18 定案）
**真**：顶层 `def`（前/后）· **程序根语句树（`ast_.label` 体）里的嵌套 `def`**（前/后、引用点在别的 def 里也算）·
顶层 `var` / `const #X` / `type` / enum 成员名 · 自己的参数与同体局部 · **stdlib 前缀里的名字**。`#if` 与裸 `if` 同规则。
**假**：某个 `def` **体内**的嵌套 `def` · 别的函数的参数 · label 名 · 内建（`store`/`raw_cast`）·
`var` 声明在引用点**之后**（`def`/`type`/`const #` 后置仍真）· 无 `#` 的顶层 `const` · 未定义名。
名字表取自**未经 `in_scope` 裁剪的原 AST**（两遍，但不必不动点）。
同一条规则的另一面：**程序根体里的嵌套 `def` 像顶层 def 一样可被前向调用**（058 的调用点在第 269 行、声明在 350 行；
参考产物的存在就是证据）—— 把嵌套 def 只绑在它的语句位置上会让这些调用报「未定义的函数」。
验证：`docs/notes/2026-10-18-ul14-in-scope.md`（45 条片段 × 参考产物字节 + `objdump` 复核；
另用 `nest.py` 套回 98 份：95 份翻转 / 190 条 (文件,名字)，40 份 TickDiff 全在名单里）
最后验证：2026-10-18

### U-L4（部分）：复合字面量 `T { a: v, … }` 的字段初始化规则
**已验**（2026-10-18）：未列字段默认 **0** · 字段按**名字**匹配（与书写顺序无关）· 求值顺序 = **源码书写顺序** ·
字段值可以用**运行期表达式**（`load` / 函数调用）。
**未验**：字段偏移 / padding / 嵌套布局 —— 三次尝试都**不可观测**（只写不读的 internal 全局被参考 DCE；
全局初始化数据不在变化页；反汇编里被常量折叠 + SROA 拆栈槽）。现有 `packed`（`ceil(bits/8)`、声明序）**这轮没改也没验**。
反向的也可观测：参考**支持**顶层 `var x = T { a: U8 (f()) }` 这种运行期初始化（我们目前报「不是编译期常量」）。
验证：`docs/notes/2026-10-18-ul4-layout.md`；器材 `work/tmp/ul4/e2.py`（探针 + 参考编译 + 宿主观测 `set_text` 字节）
最后验证：2026-10-18

### 嵌套 def 的自由变量按「**调用点**」解析，不是按 def 的文本位置
- `def` 写在 `var x` **之前**、而调用点在 `var x` **之后** → 参考**编得过**（所以不是在 def 文本位置解析）；
- 调用点在 `var x` **之前**（同一块）→ 参考**拒绝**：`Variable x not in scope. (1516)`（所以解析在调用点、顺序敏感）；
- 调用点自己查不到时，链条**往外走到最外层调用点**（程序根语句树）才命中。
⇒ 程序根体里的 `var` **不是全局**（若是全局，第二条不该报错）。我们据此改成「在调用点摊平捕获」（`ebc97c2`）。
验证：`docs/notes/2026-10-18-closure-var.md`；片段与命令 `work/tmp/in_scope/rootvar_{E_def_then_var,K_guard_before}.si` + `work/tmp/ul14/refcc.py`
最后验证：2026-10-18

### `random` 的语义（实测；它定义在前缀里，但 98 份里 **32 份**在用）
- 递推：`state ← ((x ^ (x<<12) ^ (x>>25) ^ (x>>27)) * 0x02545F4914F6CDD1) mod 2^64`
  （**前缀写的是 15 位常数**，不是教科书那个 `…CDD1D`），初值 `state = 1`；
- `random()` 返回**新 state 的全 64 位**；`random(n) = (U64)state % (U64)n` → `[0, n)`；`n==1` → 0；
  **`n==0` 返回被除数、不 trap**；`n<0` 按 U64 重解释；
- 种子是**程序自有全局，跳 tick 延续不重播种**（tick1/2/3 的读出 = state_1…state_4）；种子绝对地址未验（宿主搜不到）。
- **可以单点实现**（不需要 arena / `array` / `size` / `@Type`）：一个 8 字节全局 + 4 行算术。
验证：`docs/notes/2026-10-18-stdlib-semantics.md`（器材 `work/tmp/stdlib/**`）
最后验证：2026-10-18

### 我们的 lowering 产物在游戏内执行成功（LLVM 前端产物第一次真跑）
`manifest.tsv` seq 007 / 009：`impl=ours`、`out_len=58612`(0xE4F4)、`entry=0xE1F0`；
`hook.log`：`impl=ours mode=ours result=ok budget_ms=5000 guard=veh`，无 error 行；
游戏不崩、撤销历史可用、`sim_read` 能读到 RV32IM 的真实主频。
此前游戏内只跑过**手写 C** 的产物（M1/M1.5），这次是 `decls → expr → ir_build → LLVM → tcblob` 全链产物。
验证：`awk -F'	' '$1=="007"||$1=="009"' work/hook/manifest.tsv`；`grep -c error work/hook/hook.log` = 0
最后验证：2026-10-18

### M3 的靶子：RV32IM 电路的捕获已固定为仓内资产
`simplexc/corpus/rv32im/`：`rv32im.si`（10369 行 / 360772 B，sha256 `dba14f86…8d220e`）+
`rv32im.bin`（参考产物 219088 B，sha256 `baffee9f…60ca01`，`entry=0x215B`）+ 捕获时整份 manifest。
规模：`get_component_count() = 532`、`mode_refresh()` **6252 行**、43 个 def 名 / 89 条顶层声明；
**可达闭包 = 10 个名字 / 5090 代码行**（从 `run_sim` 顶层块起按调用图名字匹配，`decls.py` 的 strict 口径），
加顶层骨架 91 行 = **M3.0 规模 5181 行**。
⚠️ 口径坑：`work/tmp/dossier/reachability.py` 给出的「19 个可达」把 `def name(` **声明头**也当调用
（8 处单行/空体 def + 嵌套 `handle_test_result` 头），多算 9 个；以 `decls.py` 的 10 个为准。
`com_custom`(33) 与 `com_probe_wire_bit`(48) 本体是**空块、0 行代码**（代码已在 `mode_refresh` 里平铺），
所以「子元件层级」不构成额外 lowering 工作量。参考产物 ≈46600 条指令，与设计 §2.5 的 46591 条吻合。
为什么必须存仓里：`work/hook/` 每次游戏内编译都覆盖同名文件（实测 17:12 那批被 17:21 整批替换），
而且产物里的游戏内存地址按进程实例变 —— 判据不能依赖会被冲掉的路径或活着的进程。
验证：`sha256sum simplexc/corpus/rv32im/rv32im.{si,bin}`；`grep -c '' simplexc/corpus/rv32im/rv32im.si`；
      可达闭包：`python simplexc/frontend/decls.py --src simplexc/corpus/rv32im/rv32im.si`（看 stderr 的 strict 口径）
最后验证：2026-10-17

### 宿主该映射多大，由 `memory_copy`/`memory_clear` 的**实参**决定（不是 `c_alloc`）
参考产物 `+0x2391` 是 `rep movs`：`rsi = #RESET_DATA_7069020504170261741 - 0x800`、
`rdi = #DATA_7069020504170261741 - 0x800`、`ecx = 0x8001000`。源码里对应
`memory_copy(…, 134221824)` —— **134221824 = 128 MB + 4096**。
⚠️ 行尾注释里的 `c_alloc(134217728)` 只是数据大小，**照它映射会少 4096 字节**：
AV 地址 `0x18928150000` 正好落在「按 128 MB 算出的映射末尾」（实测，差一个页就崩）。
→ `simspec` 解析 `memory_copy/memory_clear` 实参算绝对区间，`host.merge_spans` 按它页对齐并合并；
RV32IM 合计 **278.5 MB / 6 段**（1 号组 128 MB×2 + 3 号组 8 MB×2 + 小的）。
验证：`PYTHONIOENCODING=utf-8 python simplexc/tools/host/selftest.py --src simplexc/corpus/rv32im/rv32im.si --bin simplexc/corpus/rv32im/rv32im.bin --entry 0x215B --ticks 10`
      → `tick 10/10 · 逐 tick 全等=是 · 仿真真在工作=是`（修复前是 AV，tick 0/0）
最后验证：2026-10-17

### 生成器给块地址的行尾都带注释，`Ptr` 解析必须容忍
`const #DATA_* / #RESET_DATA_* / #SETTINGS_*` 行尾是 `// """ & $(c_alloc(…) + 2048) & … """`。
`simspec.py` 的 `PTR_RE` 原来以 `\s*$` 收尾 → 这 12 个地址整批漏掉（地址表只有 9 个标量），
`reset_sim` 一跑就 AV。修成 `\s*(?://.*)?$` 后标量地址 9 → 21 个。
验证：`python -c "import sys;sys.path.insert(0,'simplexc/tools/host');import simspec;print(len(simspec.SimSpec.parse('simplexc/corpus/rv32im/rv32im.si').scalars))"` → 21
最后验证：2026-10-17

### simplexc/corpus：98 份唯一源码 + 含 entry 的 manifest
`work/hook/all/` 的 180 份捕获按源码字节去重 → 98 份（重复分布 1×91、2×3、3×2、4×1、73×1，
最多的是 004 出现 73 次）；离线重编译后 `entry` 全部非 0（53 个不同值，`0x212f` 出现 15 次）
且都 < `out_len`；98 份源码只产出 **97 个不同产物**（`020` 与 `058` 只差一个空行的位置）。
`corpus/manifest.tsv` 13 列：`id file src_len src_sha256 dups seen flags entry out_len out_sha256
ref_match ref_match_rebased rebased_delta`；`corpus/ref/*.bin` **不进 git**（能由源码重新生成）。
验证：`python simplexc/tools/recompile_corpus.py --check`；独立复算（不 import 该工具）行数/列数/
      `entry` 边界/`out_len` 与文件大小/`out_sha256` 与文件内容
最后验证：2026-10-16

### stdlib 前缀：27711 B 含 55 处 CRLF，归一化后 27656 B
`compile_real.dll` 偏移 `0x119F48` 起 27711 字节；CRLF→LF 归一化后 27656 B（1503 行）。
两者**等价** —— 不要去「修」这个差。
验证：`cmp simplexc/corpus/stdlib_prefix.si <工作副本>` → 相同（工作副本从 DLL 段重新提取后归一化）
最后验证：2026-10-16

## 离线差分宿主（M2）

### 游戏内存地址在全新进程里可直接 VirtualAlloc
生成源码里硬编码的游戏内存地址（`0x232a6f…`、`0x232a9f…`、`0x232aab…` 段）在一个**全新的
`python.exe` 进程里能按原地址 `VirtualAlloc(MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE)` 成功**
（6/6 个样本全成功）→ 差分宿主可以「按源码里出现的绝对地址造假内存」，不必先把产物里的地址重写。
注意：`VirtualAlloc` 会把请求地址向下取整到页边界（`0x232a3220800` → `0x232a3220000`），
拿返回值当实际基址。
验证：`python -c` 对 `work/hook/all/hook_src_001.si` 里的 `Ptr <digits>` 逐个 VirtualAlloc
最后验证：2026-10-16

### 宿主对「地址表外且已提交」的 movabs 硬错误
判据 = 值不在地址表内、且 `VirtualQuery` 的 `Protect != 0` —— **不是**「返回非空」（`MEM_RESERVE`-only 也非空但 `Protect=0`）。
命中就打印偏移/值/映射区间并非 0 退出；检查要在宿主摆假内存**之前**跑（否则自己摆的内存会造假阳性）。
每份参考产物本来有 4 处**未映射**的大整数常量（`0x2540be400`、`0x19db1ded53e8000`），它们只统计不报错。
验证：`work/tmp/iface/005_fixed.bin`（一处 movabs 改成 `0x7FFE0000`）→ rc=2 并打印；原版 005 照常跑
最后验证：2026-10-17

### 我们自己的产物靠 `<blob>.stubs` sidecar 才跑得起来
产物里的 movabs 有三种来源：`compile_real.dll` 镜像（参考产物）、我们自己的 extern 跳板、大整数常量。
跳板由装配器造（`AsmResult::stubs`），`tcblob` 写成 `<out>.stubs`（TSV `offset\tkind\tsymbol`，
kind = `stub` 12 字节代码桩 / `slot` 8 字节 `__imp_` 数据槽）；宿主读它后这些位置豁免硬错误，
并按符号接宿主桩（与 IAT 槽**同一条 handler 与记录格式**，两边 extern 序列才可比）；命中不了桩就报名字退出。
游戏内不需要 sidecar（壳自己 `GetProcAddress` 填桩）。
验证：006 的产物 6 处跳板（`Sleep` / `GetSystemTimePreciseAsFileTime` + 4 个 rt_min 的 `__imp_`）；
      `python simplexc/tools/probe_006.py --ticks 10` 靠它跑通
最后验证：2026-10-17

### 判据入口的基线：参考产物 vs 参考产物
宿主可靠性得证前不得接我们的产物（主设计 §7.2）。

### 参考产物会直接调用 compile_real.dll 里的运行时函数、读写它的 .bss
产物里的 movabs 不全是「kernel32 导入 thunk」：还有指向 DLL **内部真代码**的（RVA `0x1350`/`0x1380`，
prologue `48 83 ec 38`）与指向 DLL **.bss 全局**的（RVA `0x131280`）。所以差分宿主不能只把游戏地址摆出来
—— 必须**加载真 `compile_real.dll`**（DllMain/TLS/它自己的导入都白送），再把产物里
`[baked, baked+image_size)` 的 movabs 加上 `实际加载基址 - baked`（实测 020：23 处）。
验证：`python simplexc/tools/abs_scan.py --src simplexc/corpus/src/020.si --bin simplexc/corpus/ref/020.bin`；
      `simplexc/tools/host/host.py` 的 `_rebase`
最后验证：2026-10-17

### 导入 thunk 的真身是 `48 FF 25 rel32`，产物引用的是**指令起点**
MinGW 的导入 thunk 是 `REX.W + jmp QWORD PTR [rip+rel32]`，16 字节一格；`FF 25` 在指令第 2 字节，
而产物 movabs 用的是**指令起点**（`0x1310`/`0x1320`/`0x1370`…）。只匹配 `FF 25` 会漏掉全部 thunk。
验证：`abs_scan.py` 的 `Pe.import_thunks()`；DLL 文件 `0x1300` 处的字节
最后验证：2026-10-17

### baked 基址可从产物单独反推，不需要捕获时的会话信息
产物里 DLL 地址 = `baked_base + thunk_rva`（thunk RVA 集合从 DLL 文件本身算）→ 对每个候选 `v - t`
投票、只留 64 KB 对齐的，取票数最高者。
验证：020 得到唯一候选 `0x7ff9a1160000`（10 票）；`abs_scan.py`
最后验证：2026-10-17

### 游戏是 `jmp` 进 entry（rsp 16 对齐、栈上无返回地址）
`CreateThread(entry)` 是 `call`（rsp ≡ 8）→ 产物里对齐敏感的 SSE 指令直接 AV。宿主/壳必须复现 jmp 语义
（宿主用 `and rsp,-16; mov rax,entry; jmp rax` 的 trampoline；游戏内是我们的 `__tc_entry` shim）。
参考产物的 entry 自己就保存全部寄存器（头 16 字节 `50 51 52 53 54 55 56 57 41 50 41 51 41 52 41 53`
= push rax/rcx/rdx/rbx/rsp/rbp/rsi/rdi/r8..r11）—— 与主设计 §5 第 8 条一致。
验证：`host.py` 的 `_start_thread`；`objdump -D -b binary -m i386:x86-64` 看 `corpus/ref/*.bin` 的 entry 处
最后验证：2026-10-17

### run 的完成信号 = 命令号被消费 + 不在跑（`sim_cycle == target` 会误判）
被 `check_output`/短路 halt 的 run 会**提前结束**（实测 120：`sim_test_result=win` → halt → `sim_cycle` 停在 0）。
判据用 `sim_last_command_id == id && sim_running == 0`，且要**连续两次采样一致** —— 程序写
`sim_last_command_id` 与置 `sim_running=1` 之间有个极短窗口形状相同。
验证：`host.py` 的 `tick()`；120 从「20 s 超时」变成 10/10 通过
最后验证：2026-10-17

### 节奏类调用不能进可观测序列
`Sleep` 与读时钟的条数取决于**真实时间**（程序在两次命令之间的轮询次数；实测两次跑差 3 万~650 万条），
对仿真状态没有语义影响 → 宿主不记进 `extern`（主设计 §7.1 的显式白名单），只留计数在 `pacing` 字段。
验证：005/006 两次跑的 `extern` 条数不同，而页与 `sim_cycle` 逐 tick 全等
最后验证：2026-10-17

## 构建与 LLVM

### LLVM 固定版 = 官方 23.1.3（win64 / MSVC 构建）+ 自造 MinGW 导入库
资产 `clang+llvm-23.1.3-x86_64-pc-windows-msvc.tar.zst`（263.5 MB），
sha256 `7e69dc07a4536b79ff24a651a2611990bf8eaab17ea560f1092707c4f31ba01d`
（GitHub API 的 `digest` 与本地重算一致）。
取件：`python tools/fetch_llvm.py --dest D:/downloads` → `build/llvm/{bin/LLVM-C.dll, include/**, lib/LLVM-C.lib}`
（不进 git；`build/llvm/llvm.json` 记版本/资产名/sha256）。MinGW 导入库：
`gendef LLVM-C.dll`（1240 个导出）→ `dlltool -m i386:x86-64 -d LLVM-C.def -l lib/libLLVM-C.a`
（MSVC 那份 `LLVM-C.lib` 没用上）。运行期靠系统中已有的 MSVC 运行时
（`vcruntime140.dll` / `vcruntime140_1.dll` / `msvcp140.dll`）。
验证：`g++ -std=c++17 -O2 -I build/llvm/include simplexc/shell/llvm_link_probe.cc -o build/llvm/probe.exe -L build/llvm/lib -lLLVM-C`
      编译通过，运行输出 `RESULT: OK`
最后验证：2026-10-16

同一个包里解出的 `bin/clang.exe`（M1.5 的产物必须由 clang 生成）：
`python tools/fetch_llvm.py --offline --tag llvmorg-23.1.3 --extra bin/clang.exe`，sha256 `9584244d83bb…`。
`--offline` 用本地压缩包 + `llvm.json` 里记的 sha256 重解（GitHub 临时 API 会 403）；
顺手修了旧实现的一个 404：按 tag 查要走 `releases/tags/<tag>`（`releases/{ref}` 只对 `latest` 合法）。
验证：`build/llvm/bin/clang.exe --version` → `clang version 23.1.3 (…0d261d1ca552…)`
最后验证：2026-10-17

### LLVM 产出的 COFF：装配器的输入形状
`x86_64-pc-windows-gnu` + `RelocMode=Static` + `CodeModel=Small` + `default<O2>`：
- 节：`.text` / `.data` / `.bss`（空的也在）；有栈帧的函数额外带 `.pdata` / `.xdata`；
- `.text` 重定位**只有** `IMAGE_REL_AMD64_REL32`，目标有三类：内部函数、未定义 extern、
  **`___chkstk_ms`**（>4 KiB 栈帧才出现，是 CRT 符号不是 kernel32）；
- `.pdata` 用 `IMAGE_REL_AMD64_ADDR32NB`（指向 `.text`/`.xdata`）→ **整节丢弃**即可，别去解；
- 未定义符号在 COFF 里是 `sec 0` + `scl 2`。
验证：跑 `build/llvm/probe.exe build/llvm/probe_obj.obj` 后 `objdump -h/-t/-r build/llvm/probe_obj.obj`
最后验证：2026-10-16

### COFF 的 REL32 addend 是**有符号**的
`IMAGE_REL_AMD64_REL32` 的字段内容按有符号 32 位参与 `S + A - (P + 4)`；GCC 会用 `A = -1` 表示
「字面量地址减一」的偏置（它的逐字节复制循环从 `[r8 + i + 1]` 读回）。当无符号算，每处这类引用都会
溢出 ±2GB。
验证：`simplexc/assemble/assemble.cc` 里的 `int32_t addend = int32_t(r32(...))`；
      `build/objs/rt_min.o` 的 `.text+0x27`（字段 `FF FF FF FF`，对 `.rdata`）与 w64devkit `ld` 链接后的位移一致
最后验证：2026-10-17

### 链接产物随 `-L` 写法与 `-o` 路径变化
`-L` 写绝对路径与写相对路径时 link map 逐行相同、产物字节**不同**（import thunk 表布局变了）；
DLL 的 `-o` 换成别的路径字符串会换掉 ld 挑的默认 image base（产物差 ~150 字节）。
`simplexc/tools/build.py` 因此固定「相对 `-L` + 与 `deploy_compile_hook.py` 同一种绝对 `-o` 拼法」。
验证：`work/tmp/flagcheck/` 里两份 link map 与 md5 清单；`build.py hook` 与 `deploy_compile_hook.py build` 产物逐字节相同
最后验证：2026-10-17

### 产物必须是能驱动仿真的程序：空/桩程序会让游戏在**下一次**编译请求时 abort
游戏每条编译请求都是先 `send_command(3)` + `joinThreads()` 再 `compile()`；而一个什么都不做就
`ExitThread` 的产物会让仿真协议处于半途，下一次编译请求就在游戏自己的状态机里 fatal 退出。
特征（用于与真崩溃区分）：**不产生 WER 报告、不产生 CrashDump**，且因死在 `compile()` 之前，
`work/hook/` 里连 `hook_src_NNN.si` 都不会新增。
验证：2026-10-17 游戏内实测（`compiler = ours` 返回 728 字节空程序 → 下一次手动编辑触发编译时 abort；
      `%LOCALAPPDATA%\CrashDumps` 与 WER ReportArchive 无新记录，`manifest.tsv` 停在 seq 006）
最后验证：2026-10-17

## 组件映射

### 映射原理
每个组件代码块前插 `store(唯一地址, 唯一魔数)`，编译后在机器码里搜魔数立即数定锚点，
再坐标变换回无标记版本。标记恒 **17 字节**，变换契约 `pos_无标记 = pos_带标记 − 17×(i+1)`。
验证：静态核对（`tools/build_component_map.py`）
最后验证：2026-10-11

### 组件区之后必须插终点标记
它界定最后一个组件的结束，后果见 `tools/build_component_map.py` 文档头。
验证：静态核对（`tools/build_component_map.py`）
最后验证：2026-10-11

### 每个组件 ID 在源码里出现两次
组件代码 + 绑定各一次，所以「ID 不重复」不能用来判断完整性。游戏源码里每个组件本来就有
**2 条注释**（两段各生成机器码），正确做法是**按 `comp_id` 聚合**而非按注释行。
验证：静态核对（源码注释直方图）
最后验证：2026-10-11

### 完整性判据
唯一可靠的是源码自报的 `get_component_count()`（顶层组件数）与组件注释唯一 ID 数比对。
验证：静态核对
最后验证：2026-10-11

### 建映射不再污染捕获目录
`try_compile_dll.load_dll()` 默认 `prefer_real=True`，优先加载 `compile_real.dll` 而非转发层，
不会往 `work/hook/` 写新捕获；只有显式 `--hook` 时才走转发层。
验证：静态核对（`tools/try_compile_dll.py:39,53`）
最后验证：2026-10-11

## 采样

### RIP 判据
「RIP 是否落在目标机器码的**字节范围内**」（旧判据「是否落在非 IMAGE 可执行区」不够精确）。
实测 8773 个采样点中 8770 个在范围内（99.97%），3 个越界点全是同一个系统 dll 地址。
验证：跑 `python tools/profile_by_image.py`，看落在范围内的比例
最后验证：2026-10-11

### 多副本
同一份机器码在内存里可能同时存在多个副本，**每编译一次占一个新 JIT 区且旧区不释放**。
实测：19 份相同产物 ↔ 19 个不同 JIT 基址的副本（1:1），只有地址最高的最新副本在执行。
验证：跑剖析，数内存里匹配的副本数
最后验证：2026-10-11

### `com_none` 不是真组件
它是空注释；区间归属错误，把注释之后的公共代码算给了它，所以「平均字节」列无意义。
验证：静态核对（现成产物结构吻合）
最后验证：2026-10-11

## 机器码改写

### 四态与两条规则
`rewrite = on / short / short_cse / off`。规则①：`mov ecx,imm32` + `移位 r,cl` →
`移位 r,imm8`（恒缩 4 B）；规则②**仅 `short_cse`**：删紧邻重复装载（`short` 的 CSE 列表恒为空）。
验证：静态核对（`optimize_image.py:676` `cse = find_cse(...) if mode == "short_cse" else []`）
最后验证：2026-10-11

### 填充一律用单条多字节 nop
理由见 `optimize_image.py` 的 `NOPS` 表注释。
验证：静态核对（`optimize_image.py` 的 NOPS 表）
最后验证：2026-10-11

### 收益来自去掉「变量移位」依赖链
不是指令数量本身。旁证：`v_off.bin` 与 `v_onnew.bin` **都是 50403 条指令**却主频不同。
验证：对比两份产物的指令条数与主频（历史实测）
最后验证：2026-10-11

### 模式传递链
`rewrite` 的值只被读一次、只被判定一次；转发层只负责原样搬运（原样写 `hook.log`、
原样经 `--mode` 传），**合法值集合只在 `resolve_mode()` 的 `VALID_MODES` 定义**。
验证：静态核对（`compile_hook.c` + `optimize_image.py`）
最后验证：2026-10-11

### `opt.ini` 规则
不在 git 里；切换靠搬注释符；**若多行都放开：取第一个匹配行生效、不报错**
（读法为何必须与转发层一致、旧实现错在哪，见 `resolve_mode()` 的注释）；
非法值与读不到时都按 `on` 兜底。
验证：静态核对（`optimize_image.py:607-629`）
最后验证：2026-10-11

### 日志的语义与格式
`work/hook/hook.log` 是**唯一**的记录型日志（2026-10-18 从 `work/opt/` 迁来），转发层与改写子进程共写、追加写；
Python 侧取路径走 `tools/hook_log.py` 的 `resolve()`：两文件只存在一个就用它、都在取 mtime 更新的、
都不存在则新路径，且 **`append()` 写到刚刚 `read()` 到的那个文件**（窗口期不分裂 `cfg`/`opt`）；
每行 `时间 \t 编号 \t 事件 \t key=value…`（事件表与键表见 `tools/hook_log.py`，
C 侧实现是 `compile_hook.c` 的 `hook_log()`）。`cfg` 行 = 配置了什么（原样搬运），
`opt` 行 = 实际执行的模式 / 改写处数 / 产物 sha256 —— 两者不一致就是 short 退回了 on。
`impl` 行 = **实际走了哪个实现**（每次编译都写、含 reference），键 `impl`/`mode`/`result`（ours 行可选 `budget_ms`、
首次加 `guard=veh`）；⚠️ `impl` 行的 `mode` 是配的 `compiler`，与 `opt` 行的 `mode`（改写器实际做了什么）**不是一个东西**；
词表唯一出处是 `docs/specs/2026-10-15-simplexc-interfaces.md` §5。
`opt_stderr.log` 是子进程 stdout/stderr 的原样重定向（多行、非结构化、**GBK 编码**，
不是 UTF-8），**不是**记录项，单独放。
验证：静态核对（`tools/hook_log.py` + `compile_hook.c`）+ 游戏内 10 次编译实测：
`hook.log` 32 行全可解析（无坏行）、每次 `cfg`/`opt`/`writeback` 齐全、无 `error` 行、
`opt` 行的 sha256 与 `NNN.opt.bin` 逐条相符、`entry` 与 `NNN.entry.txt` 相符
最后验证：2026-10-15

## 主频读数

### UI 上那个 MHz 的来源
`sim_speed__presenterZutilities_u29531` —— `.bss`，链接地址 `0x14624d618` → 模块内偏移
**`0x624D618`**，float32，单位 Hz。由游戏每秒重算一次，算法 `(cycle 增量)/(时间增量)`；
紧邻 `last_cycle_count`（`0x624D608`，int）与 `last_speed_check`（`0x624D610`，Unix 时间戳 double）。
验证：`python tools/read_freq.py`；旁证 `work/freq_1.txt:4-10`（内存值与 cycle/时间自算逐条一致）
最后验证：2026-10-11

### 采样判据四条
① 暂停/停止 → `sim_speed` 归 0（实测停止后 +1.11 s 起恒为 0）；
② 电路自带限速时停在上限——是**非 0 的极低值**，不能当噪声丢；
③ 启动/停止那一秒读到的是**混合值**（窗口横跨切换点），统计要排除；
④ **停机时 `last_cycle_count` 冻结，但 `last_speed_check` 仍每秒刷新**（逐秒 +1.000~1.006 s）
→ **不能用时间戳判断仿真是否在跑**，要看 `cycle` 是否前进。
验证：`python tools/read_freq.py --watch`；④ 另有直读内存复核
最后验证：2026-10-11

## simctl

### 三条设计原则
① 不做自动检测（不监听 `opt.ini` 的 mtime）；② 不判断编译是否成功；③ 每次调用只发一个键。
验证：静态核对（`simctl.py:20-21`）
最后验证：2026-10-11

### 三个动作的机制
`run()` **注入 F6**（运行/暂停**切换**，非幂等）；`stop()` **注入 F4**（停止/复位）；
`recompile()` 注入 Ctrl+Z / Ctrl+Shift+Z。
验证：静态核对（`simctl.py:401-411`）
最后验证：2026-10-11

### 抢前台
有效手段 = 先 `SendInput` 发一次 Alt 解锁，再 `AttachThreadInput` 拼接输入队列。
**例外**：调用方是当前焦点进程或它的子进程时不需要 Alt（此时裸调用也成功）；
处于其他进程则必须走 Alt 路径。
验证：对照实测——前台是无关进程时裸调用返回 FALSE、前台不变；前台是本进程所在终端时返回 1
最后验证：2026-10-11

### 抢前台的前置校验与拦截
发键前必须校验前台就是游戏窗口，否则拒绝；有可见 `#32770` 错误框时一律不操作。
主窗口识别 = 「可见 + 有标题 + 非 `#32770` + 无 owner」。
验证：静态核对（`simctl.py:239-241, 351-354, 390-393`）
最后验证：2026-10-11

### recompile 的判据与记账
判据是 `next_sim_version`（`.data 0x14049c300`）**每次编译请求 +3**（游戏内部一次撤销固定发
3 次请求；实测变化序列 44→46→47 是分次 +1），纯读内存。撤销是消耗性操作，记**净消耗步数**，
退出时按净消耗补发等量反向键。
验证：读内存 → 发一次 recompile → 再读，差值应为 +3
最后验证：2026-10-11

## 静态逆向

### DWARF 节不含 Nim 信息
`compile_real.dll` 的 `.debug_info` / `.debug_line` 是 MinGW CRT 的：`objdump --dwarf=info`
得 25 个 CU、含 `nim` 的 **0 个**、`OOZ` 出现 **0 次**。
验证：`objdump --dwarf=info compile_real.dll | grep -ci nim` 应为 0
最后验证：2026-10-11

### `scan_game_mem.py` 的截断风险
存在真实截断路径（窗口与停止条件见该脚本），历史消费者用「`{` 与 `}` 数量相等」做拦截；
但从未观测到实际不平衡（现存源远小于窗口）。
验证：静态核对（`tools/scan_game_mem.py:205,212,220-221,230,237`）
最后验证：2026-10-11

### 契约重验脚本（Steam 更新后先跑它）
`python simplexc/tools/verify_game_contract.py`：按**符号名**定位（自己解析 PE 的导出表/COFF 符号/`.pdata` 取函数边界，
不复用 `work/symbols/*` 快照），断言 A–E：`compile` 导出 + 全 exe 唯一调用点 + `flags` 置 0 / `jit` 内 memcpy(`ptr+8`) 与尾跳 /
`handle_request_compile_and_run` 内 `out[3]` 的 test+jne 失败分支（跳过 `send_command`/`createThread`）/ `eqdestroy` 的
`testb $0x40,0x7(p)` + `rawDealloc` bit62 门控 / 产物头除 bit62 外不被读。
退出码：无 FAIL=0、有 FAIL=1、跑不起来=2。E 是**否定性命题**，静态判定不了 → 恒 `UNKNOWN`（真判定要动态实验）。
实测当前 buildid 25294874：A–D PASS、E UNKNOWN。
验证：跑该脚本看每条断言的定位方式与反汇编证据；变异自检 `work/tmp/negtest/make_mutants.py`
最后验证：2026-10-17

## 装配器与产物确定性

### 产物逐字节可复现的前提：不读「没有原始数据」的节
COFF 对象文件里 `.bss` 是 `SizeOfRawData=N / PointerToRawData=0` —— 文件里没有它的数据（内容隐含全 0）。
装配器若按 rawptr 读 rawsize 字节，读到的是**文件开头的 COFF 头**（Machine + NumberOfSections + TimeDateStamp）
→ 产物带编译时间戳（同一源码两次编译的 blob 不同）且 `.bss` 初值是垃圾。
修法：`raw_ptr==0` 的节留空（payload 本来就零初始化）；节大小 = `max(SizeOfRawData, VirtualSize)`。
⚠️ 对象文件里 `VirtualSize` 实测恒为 0（clang 与 gcc），大小在 `SizeOfRawData`。
验证：同一份 `simplexc/probe/006_equiv.c` 用 clang 连编两次（两个 `.obj` 只差 header 时间戳 1 字节）
      → `build/tcblob.exe` 装配出的两个 blob 逐字节相同；blob 内搜输入 `.obj` 的 COFF 头/时间戳 0 命中
      （修复前的对照与脚本见 `docs/notes/2026-10-17-assemble-bss.md`）
最后验证：2026-10-17

### `.obj` 本身不可复现（clang 每次写 COFF TimeDateStamp），`SOURCE_DATE_EPOCH` 无效
所以「产物确定」只能在**装配后的 blob** 上说；比 sha / 做 A/B 记录都要拿 blob，不要拿 `.obj`。
验证：连编两次 `cmp -l` 只差 offset 4 的 1 字节；设 `SOURCE_DATE_EPOCH=0` 两次仍是不同时间戳
最后验证：2026-10-17

## M1.5 收益探针（006）

### 逐 tick 全等：clang -O2 等价物 == 参考产物
`simplexc/probe/006_equiv.c`（手写 006 等价物，自实现整套仿真协议）经
`clang --target=x86_64-pc-windows-gnu -O2` → 装配 → 宿主 10 tick 比对参考产物：
页哈希 / 状态字段 / extern 序列逐条相同，**白名单为空**。
验证：`python simplexc/tools/probe_006.py --ticks 10`（fold 与 hot 两变体都 10/10、差异 0 条）
最后验证：2026-10-17

### 收益来自「热状态进寄存器」，不是「直译成 C 交给 clang」
同驱动配对量每 cycle（大 N 下每爆发开销相消）：参考 1.37 ns、直译 fold 1.71 ns（慢 ~25%）、
把每周期计数器先放局部变量再写回的 hot 变体 0.467 ns（快 ~2.95×，语义等价且逐 tick 仍全等）。
机制：游戏内存是 `volatile`，`cycle` 是 u64 全局 → LLVM 不敢假设两者不别名，只能每周期溢出到内存。
**对 M3 的指示**：lowering 要让私有热状态有「有序但不与游戏内存别名」的语义，照抄全 volatile 会把收益吃光。
⚠️ **上面三个数字全是宿主里的**：游戏内（真 Sleep、真时钟、游戏设的 target_cycle）**从未量过**，M1.5 已于
2026-10-17 终止（不再做游戏内 A/B），所以「换编译器在游戏里能提速」目前**没有证据**，只有这一条宿主相对结论。
验证：`simplexc/probe/results/006_{fold,hot}.{json,tsv}`（含每点 5 轮原始秒数、pacing 计数、拟合残差）
最后验证：2026-10-17

### 006 的语义（从参考产物实验得出，别重猜）
- `Time` 单位是**纳秒**（`33.ms()` = 33e6 ns）。
- 宿主驱动的每个 tick 只推进 **1 个 cycle**（`sim_target_cycle = 1..N`）→「每 tick 墙钟」量的是宿主粒度。
- `#if in_scope("get_input" / "check_output" / "on_ui_update")` 三处在 006 里都**不生成代码**。
- `mode_refresh()` 从内存回读刚写的位再算 NAND（不是用局部变量）。
验证：`docs/notes/2026-10-17-m1.5-probe.md` §「从参考产物实验出来的 006 语义」
最后验证：2026-10-17
### LLVM 的代码生成级别与 IR 优化是两件事 —— Python 原型一直按 -O0 出机器码
`LLVMCreateTargetMachine` 的 `CodeGenOptLevel` 枚举是 `None=0, Less=1, **Default=2**, Aggressive=3`。
Python 原型 `lower/ir_build.py:213` 写的是 `self.CodeGenLevelDefault = 0`（= None），
所以它的 `LLVMRunPasses(default<O2>)` **只优化 IR**，机器码生成始终是 -O0。
实测：同一份 `rv32im.si` 的 IR **逐字节相同**（md5 `869677f0891a7884bcf025fc6384937e`，1379416 B），
Python 产物 `.text`=0xE1E9(57833)/COFF 59047，C++ 产物 `.text`=0x6393(25491)/COFF 26843，
而 clang `-O2` 编同一份 IR 得 0x6393、clang `-O0` 编**已优化**的 IR 得 0xE1E9 —— 三路互证。
含义：M3/M4 的所有 Python 产物（58612 B、游戏内 24.83 MHz）都是 **-O0 代码生成**的成绩；
「C++ 产物小 2.2 倍」不是写法差异，是这一处枚举值。
验证：`python work/tmp/check_py_o2ir.py`（落盘优化后 IR + clang 两种 -O 对照）；`docs/notes/2026-10-18-m45-cli.md`
最后验证：2026-10-18

### M4.5：C++ 前端 + lowering 已能独立产出等价 blob
`build/simplexc_cli.exe --src <si> --out <blob>`（`python simplexc/tools/build.py simplexc_cli`）是产品路径入口：
frontend → `ModuleAst` → `lower::Program` → `assemble()` → blob，不经 Python、不碰游戏。
- **逐 tick 等价**：`rv32im.si` → 26340 B / entry `0x63A0`，与参考产物（`0x215B`）10 tick **差异 0 条**；
- **延迟门**：`corpus/src/177.si`（479225 B，最大语料）前端 38 ms + lowering 13 ms + LLVM(O2+代码生成) 212 ms + 装配 1 ms
  = **266 ms**（门 ≤4 s）；外部 wall 479 ms（含进程启动与文件 IO）；
- **依赖门**：`objdump -p build/compile_hook.dll | grep -i python` 为空（导入表只有 KERNEL32/msvcrt）。
验证：`docs/notes/2026-10-18-m45-cli.md` 的命令与输出；`python simplexc/tools/build.py simplexc_cli`
最后验证：2026-10-18

### U-L3：模板字面量插值 = 按序拼接各值的渲染，不调运行时、不需要 NUL 终止
实测（参考编译器产物 + 宿主 `--dump-pages` 观测语料自带的 `set_text`→`ui_buffer` 写入）：
`A{Int 1234}C` → 6 字节 `A1234C`；`{U64 max}` → 无符号十进制；`{Bool true}` → `true`；
`{U1 1}` → `1`；`{Str "hi"}` → `hi`；多段插值按出现顺序拼接；`"ab"+"cd"` → len 4 的 `abcd`。
**extern 序列为空**（不调运行时）、结果指针落在调用方线程栈上、长度显式 → **不需要 NUL 终止**。
验证：`docs/notes/2026-10-18-m5-gap-diagnosis.md` 的命令与页快照读数
渲染规则（2026-10-18 逐类型实测，探针 `work/tmp/m5interp/probe.py`）：
`Int`/`S*` 按**声明位宽符号扩展**后取有符号十进制（`{S64 min}` 也对）；`U*` 无符号十进制（窄无符号不做符号扩展）；
**`Bool` 渲染 `true`/`false`，而 `U1` 渲染 `1`/`0`** —— 同一台机器上两种渲染；`Str` 原样；`Ptr` 是地址的无符号十进制；
运行期值同样按**声明类型**渲染。`+` 拼接 = 长度相加 + 逐字节。
`I8`/`I32` **不是合法类型拼写**（只有 `^[US]\d+$`）；**枚举插值参考编译器自己 fatal**（`front_end.nim(2465,5)`）→ 语料里不存在这个形状。
验证：`docs/notes/2026-10-18-str-interp.md`（含每类的探针与读出字节）
最后验证：2026-10-18

### 参考编译器容忍 `return <expr>)` 里多余的右括号（我们比它严）
实测（2×2 单因子）：`store(...))` 多一个 `)` → 参考 `sysFatal front_end.nim(2688,28) Missing open paren`；
`return …)` 多一个 → **编译成功**，且产物 sha256 与删掉它的同一份源码相同（`f41b2839b25cf6a9…`）。
17 份语料全落在这个被容忍的形状（每份唯一不平衡行都含 `return`、每行净 −1）。
验证：`docs/notes/2026-10-18-m5-gap-diagnosis.md`
最后验证：2026-10-18

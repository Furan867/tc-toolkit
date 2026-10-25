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
⚠️ memcpy 读到 `out_ptr + 8 + out_len`，比缓冲末尾多 8 字节（原版编译器靠 Nim 字符串余量兜住；
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

## 原版编译器 compile_real.dll（编译器 / 产物侧）
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

### stdlib 里 `kernel32.'Name'` 的真实绑定：三类，只有两类要名字映射

参考产物里这些调用编成 `movabs reg, imm64` + `call reg`。**算法**：拿 `imm64` 减去该产物里
`compile_real.dll` 的加载基址得 RVA。98/98 份参考产物五个 RVA 完全相同；另一次 boot 的捕获
（基址不同、`imm64` 全变）RVA 一个不差 —— 所以下面的表不是噪声。

| stdlib 里的名字 | RVA | 落在 | 最终绑定 | 该怎么办 |
| --- | --- | --- | --- | --- |
| `_virtual_alloc` | `0x1310` | `.text` 导入 thunk（`48 FF 25 …` → IAT `0x148370`） | `KERNEL32!VirtualAlloc` | 名字映射 |
| `_virtual_free` | `0x1320` | 同类 thunk（IAT `0x148378`） | `KERNEL32!VirtualFree` | 名字映射 |
| `_get_system_time_precise_as_file_time` | `0x1350` | **DLL 自己的代码**：无参 → 64 位 FILETIME 的包装（`sub rsp,0x38; lea rcx,[rsp+0x28]; call [IAT 0x148340]; mov rax,[rsp+0x28]; ret`） | 包装二跳 → `KERNEL32!GetSystemTimePreciseAsFileTime` | **不要映射**，要语义垫片 |

阳性对照（证明「thunk vs DLL 代码」这个区分不是猜的）：`Sleep` → RVA `0x1370`、thunk；
`ExitThread` → RVA `0x1380`、DLL 包装。两种形态都能识别出来。

**推论**：`sema/analyze.cc` 的 `extern_sigs()` + `HExternShape::OutParam64`（垫片在
`codegen.cc`）已经就是这个形态 —— 前两个走名字映射，第三个走「无参 → 输出 64 位」的垫片，
不必改。唯一可议：`_virtual_free` 我们现在声明成 `void`，而参考那个槽是裸 thunk、`eax` 带 BOOL；
目前没人读它的返回值，所以不改。

⇒ 「这三个名字在 kernel32 / ucrtbase / msvcrt 的**导出表**里都查不到」不等于「缺功能」：
它们本来就是 DLL 内部的转发层，查导出表当然查不到。
验证：探针与原始输出在 `work/tmp/k32probe/`（临时目录）；方法就是本节的「`imm64` 减加载基址」；
`ctypes` 查导出表那版见 `work/tmp/res-k32/report.md`
最后验证：2026-10-25

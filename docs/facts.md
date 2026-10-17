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

### 判据入口的基线：参考产物 vs 参考产物
宿主可靠性得证前不得接我们的产物（主设计 §7.2）。

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

### LLVM 产出的 COFF：装配器的输入形状
`x86_64-pc-windows-gnu` + `RelocMode=Static` + `CodeModel=Small` + `default<O2>`：
- 节：`.text` / `.data` / `.bss`（空的也在）；有栈帧的函数额外带 `.pdata` / `.xdata`；
- `.text` 重定位**只有** `IMAGE_REL_AMD64_REL32`，目标有三类：内部函数、未定义 extern、
  **`___chkstk_ms`**（>4 KiB 栈帧才出现，是 CRT 符号不是 kernel32）；
- `.pdata` 用 `IMAGE_REL_AMD64_ADDR32NB`（指向 `.text`/`.xdata`）→ **整节丢弃**即可，别去解；
- 未定义符号在 COFF 里是 `sec 0` + `scl 2`。
验证：跑 `build/llvm/probe.exe build/llvm/probe_obj.obj` 后 `objdump -h/-t/-r build/llvm/probe_obj.obj`
最后验证：2026-10-16

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
`work/opt/hook.log` 是**唯一**的记录型日志，转发层与改写子进程共写、追加写；
每行 `时间 \t 编号 \t 事件 \t key=value…`（事件表与键表见 `tools/hook_log.py`，
C 侧实现是 `compile_hook.c` 的 `hook_log()`）。`cfg` 行 = 配置了什么（原样搬运），
`opt` 行 = 实际执行的模式 / 改写处数 / 产物 sha256 —— 两者不一致就是 short 退回了 on。
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

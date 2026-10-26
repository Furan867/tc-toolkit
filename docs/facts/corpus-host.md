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



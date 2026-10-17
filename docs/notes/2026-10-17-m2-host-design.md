# M2 差分宿主：设计与取证（2026-10-17）

协议本体在 `docs/specs/2026-10-15-simplexc-interfaces.md` §6（冻结）。本文件只记**实测取证**与
由它决定的实现选择 —— 那条协议默认宿主身边没有真 DLL，而实测发现这一点得改。

## 判据（M2 完成 = 宿主可靠性得证）

同一份参考产物跑两遍（两个独立宿主进程），**逐 tick 快照逐字节相等**，且不是空跑：
tick 数 > 0、每 tick 有变化的页、extern 序列不为空。空跑的"全等"没有意义，所以三条防伪检查是判据的一部分。

## 结果（2026-10-17）

`python simplexc/tools/host/selftest.py --src <si> --bin <bin> --ticks 10`：

| 语料 | 结果 | 说明 |
|---|---|---|
| 005 / 006 / 120 | **通过** | 逐 tick 全等 + 有工作页（120 靠修握手才过，见下） |
| 020 / 026 | 逐 tick 全等但不计过 | 电路太小：只有驱动方自己的 `commands`/`settings` 页在变 |
| 001（地图电路） | 崩在「写地址 0」 | 假状态保真度不够：游戏给的指针/结构没被填 |

宿主自身踩到的两个真 bug（都已修，机制进了 `facts.md`）：

1. **没做重定位** → 产物读 `baked+0x131278`（DLL 的 `.bss`）直接 AV。分类做了、改字节漏了。
2. **握手条件太窄** → 要求 `sim_cycle == target`。被 `check_output` 判 win 的 run 会 `halt()` **提前结束**
   （实测 120 停在 0），于是正常完成被当成超时（刷 650 万次 `Sleep`）。

另：`CreateThread(entry)` 是 `call`（rsp ≡ 8），游戏是 `jmp`（rsp 16 对齐）→ 必须用
`and rsp,-16; mov rax,entry; jmp rax` 的 trampoline，否则对齐敏感的 SSE 指令直接 AV。

## 实测：参考产物的绝对地址分四类（`simplexc/tools/abs_scan.py`）

以 `corpus/src/020.si` + `corpus/ref/020.bin` 为例（35 处 movabs / 15 个不同立即数）：

| 类别 | 数量 | 例 | 宿主怎么处理 |
|---|---|---|---|
| 游戏数据段 | 6 | `0x232a6ff0000` | 自己 `VirtualAlloc` 到原地址（实测可行） |
| DLL 导入 thunk | 3 | `0x7ff9a1161310` = `KERNEL32!VirtualAlloc` | 让 thunk 自己跳 IAT 槽，把槽换成宿主桩 |
| **DLL 内部代码** | 2 | `0x…1350` / `0x…1380`（真 prologue `48 83 ec 38`） | **必须执行真 DLL 的代码** |
| DLL 的 `.bss` 全局 | 1 | `0x…131280`（`.bss` @0x131000+0x280） | 同上，需要真 DLL 的数据段 |
| 大整数常量 | 3 | `10^10` / `10^13` / `116444736000000000` | 原样保留 |

**结论（改掉了"私有镜像"的想法）**：参考产物会**直接调 `compile_real.dll` 里的运行时函数**并读写它的
`.bss`，所以宿主不是"把地址摆出来"就够 —— 它得让那些代码和数据是活的。于是宿主
**`LoadLibrary` 真 DLL**，再把产物里 baked 基址重定位到实际加载基址（差一个 delta）。这同时白送三样东西：
DllMain 跑过、TLS 就位、DLL 自己的导入已解析。

## 实测：DLL 侧的可用性（宿主进程内已验证）

- 游戏数据段 `VirtualAlloc(0x232a6fe0000, 59.9 MB)` 成功（Python 进程里也没有碰撞）。
- `AddDllDirectory(游戏目录)` + `LoadLibraryExW(compile_real.dll, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`
  成功，实测加载基址与文件 ImageBase 无关（ASLR）→ **必须重定位**。
- IAT 槽里是真实内核地址（`ExitThread` → kernel32 内），`.bss` 全局为 0 → 可以直接改槽做 detour。
- thunk 的真身是 `48 FF 25 rel32`（REX.W 前缀）；**产物引用的是指令起点**（16 字节一格），
  这是第一版 thunk 识别漏掉它们的原因。

## baked 基址怎么反推（不依赖捕获时的会话）

产物里指向 DLL 的 movabs = `baked_base + thunk_rva`（thunk RVA 集合从 DLL 文件本身算）。
对每个候选 `v - t` 投票、只保留 64 KB 对齐的，取票数最高者。实测 020 得到唯一候选 `0x7ff9a1160000`。

## 驱动的握手（从语料实测，不猜）

`corpus/src/020.si` 的顶层块：`run_sim: while true { … while get_command(ctl_command_id) == last_command_id
{ sleep(1ms) } … last_command_id = get_command(ctl_command_id); set_setting(sim_last_command_id, …);
switch SimCommand get_command(ctl_command) { case run { target_cycle = get_setting(sim_target_cycle);
set_setting(sim_running,1); mode_run(target_cycle); set_setting(sim_running,0) } … } }`

→ 一个 tick = 写 `settings[sim_target_cycle] = N`、`commands[ctl_command] = run`、`commands[ctl_command_id] = ++id`，
然后**等 `settings[sim_last_command_id] == id` 且 `settings[sim_running] == 0`（连续两次采样一致）**。

⚠ 一开始写成「等 `sim_cycle == N`」，**是错的**：被 halt 的 run 会提前结束（见上「结果」第 2 条）。
也不能只看一次 `last==id && running==0`：程序写 `sim_last_command_id` 与置 `sim_running=1` 之间有个极短窗口
形状相同（只有几条内存操作，跨不过两次轮询）。

`sim_cycle` 在 `020.si:292` 处每 burst 写一次；`reset_sim` 把它置成 `#U64_HIGH`（-1）。

## 为了让两遍跑出一样的结果，宿主必须中和这些导入

| 导入 | 宿主桩 |
|---|---|
| `ExitProcess` | 只杀本线程（否则宿主进程没了） |
| `ExitThread` | 记账后真 `ExitThread(0)` |
| `Sleep` | 空操作 |
| `GetSystemTimePreciseAsFileTime` | 假时钟（每次调用 +10 ms）→ 节奏判断确定化 |
| `VirtualAlloc`/`VirtualFree` | 确定性 bump 分配器（首地址固定、16 对齐）；`addr != 0` 才转真 |
| `WriteFile`/`puts`/`fwrite`/`_write`/`vfprintf` | 记账（含载荷前若干字节），返回"成功" |

其余（`malloc`/`memcpy`/`memset`/`VirtualProtect`/`VirtualQuery`/…）保持真实 —— DLL 的运行时自己要用。

分配器确定性是**自比对的前提**：否则 `VirtualAlloc` 返回的地址会让存进游戏内存的指针值两遍不同。

## 未解决的（留给 M2 之后）

- **假状态保真度**：现在只做「零填 + 数组头（len 4096 / cap_log2 13）」。凡是要解引用游戏给的指针/结构的电路
  （001 那类）跑不了 —— 001 崩在「写地址 0」。这是 M3 全语料差分的前置。
- ours vs reference 的差分：两边分配序列不同 → 存进游戏内存的指针值必然不同，要么列白（把指针值归一化），
  要么让两边用同一条分配轨迹。M2 的自比对不涉及这个。
- 程序写 `ui_buffer`/`error_buffer` 的文本内容会随字符串地址变化 —— 同上。
- 跑完一个 run 后程序并没停（它在外层循环里刷 UI、写状态），快照靠「连续两次采样一致」等稳定；
  实测能稳定，但这是**时序假设**，不是硬保证。

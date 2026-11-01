# 元件工坊的与门（040）→ 可部署的 LLVM 产物（同会话闭环）

日期：2026-10-17 · 子 agent · 范围：`simplexc/tools/gen_addrs.py`、`simplexc/tools/equiv_build.py`、
`simplexc/probe/equiv_cc_and.c`（新建）+ 本文件

## 结论（先看这个）

1. **040 的等价物在宿主里与参考产物逐 tick 全等**：10/10 tick、差异 0 条、白名单为空。
   我们那份 blob sha256 = `b61342fff8098264e1b39dc9285f0403fc26eddf457278f91ce30bff9a5af127`，
   `.entry = 0x620`，extern 跳板 6 处。参考产物 12867 B / 3046 条指令，我们 2356 B / 541 条。
2. **地址必须按会话现取**，已经固化成脚本：`gen_addrs.py` 从任意 `hook_src_NNN.si` 抽出 11 个地址写 C 头，
   名字集合不对就报错退出。同一份脚本喂 006 得到的是 006 那套地址（`0x232A6FF0000`…），
   喂 040 得到本次会话那套（`0x210AEE20000`…）—— 这是「按会话提取」的直接证明。
3. **可复用命令序列（地址变了也不用改）**：
   ```
   cd tc-toolkit
   python simplexc/tools/equiv_build.py --src work/hook/hook_src_040.si \
       --bin work/hook/hook_out_040.bin --entry 0x212F --workdir work/tmp/equiv --mark
   ```
   `--entry` 可省（从 `--bin` 同目录的 `manifest.tsv` 查；捕获表里那一列叫 `r2`、corpus 表叫 `entry`）。
   换电路只换 `--src/--bin`；产物在 `<workdir>/ours.bin`（`.marked` 是带标记的那份）。
4. **⚠ 装配器有一个真 bug（不在本任务允许改的范围内）**：`__imp_X` 数据槽的地址在
   「遇到第一个引用」时就算定（`assemble/assemble.cc` 的 `place_for`：`body_end + stubs.size() + slots.size()`），
   之后再往**代码桩块**里追加，会把已记下的槽地址整体挤歪 —— 记进 `res.stubs` 的偏移和产物里
   `mov rax,[rip+槽]` 的位移一起歪。实测（标记版的第一次构建）：sidecar 说三个槽在 `ae0/ae8/af0`，
   真身在 `afc/b04/b0c`（差一个桩块 28 B），宿主跑它在 `entry+0x14E` 崩。
   **触发条件**：同一个产物里「`__imp_*` 引用」排在「裸名 extern 引用」前面。
   现在的绕法：`equiv_cc_and.c` 的标记路径用**裸名** extern（只往桩块追加，地址不再移动），
   并在 `equiv_build.py` 里加了「sidecar 偏移重复就硬错误」的守卫。
   这会影响主线：将来前端 lowering 若给 kernel32 用 `dllimport`（产 `__imp_`）、stdlib 里又有裸名
   extern，任何顺序都可能踩到 → 建议派一个专门的话修 `place_for`（一次算完布局、或延后到拼接前再定址）。
5. **这份电路在宿主里的可观测量很窄**：`com_cc_input` 的两个输入只可能来自 `input_replay[0]/[1]`，
   元件工坊里它们是 0 → 写进 `#SIMULATION_STATE` 的 6 个字节全是 0 → **页哈希看不到任何变化**；
   与门结果也没有落点（`com_cc_output` 在这一份里是空块）。所以逐 tick 比较实际上比的是
   协议层（`sim_cycle` / `sim_last_command_id` / `sim_running` / `sim_test_result`）+ 两个 driver 页。
   由此两条推论：
   - `selftest.py` 的 `sim_made_work` / `not_vacuous` 对这份电路**恒假**（唯一变化的页就是 commands/settings 页）。
   - 游戏内 A/B 想看出收益，得换「输入真的在变」的电路（这一份是 0 输入、0 输出，量不到电路工作量）。

## 做了什么

| 文件 | 职责 |
|---|---|
| `simplexc/tools/gen_addrs.py` | 从 `.si` 抽 11 个绝对地址 → C 头（`--src`/`-o`）。只认固定 11 个名字：缺、多、重名都 `SystemExit`。 |
| `simplexc/probe/equiv_cc_and.c` | 040 的等价物。框架照 `006_equiv.c`，电路按 `hook_src_040.si` 的 `mode_refresh`/`mode_run` 逐行对齐；地址全走 `addrs.h`；`memory_clear(..., 264)`；无 `level_*` 变量（`Input`/`Output` 是空结构）。`-DTC_MARK` 时进 `tc_main` 先追加一行到 `work/tmp/equiv/equiv_cc_and.log`。 |
| `simplexc/tools/equiv_build.py` | 一条命令：`gen_addrs` → `clang -O2 -I <workdir>` → `tcblob` → 两遍宿主 → `selftest.compare` 逐 tick → 静态指标 → 结果 JSON。`--mark` 另编一份并**直接观察**标记路径（见下）。 |

几处「为什么这么写」：

- **标记版为什么要靠另一个观察通道**：宿主快照里的 `extern` 只含**这一个 tick 窗口内**的调用，
  而标记发生在产物线程刚起来、还没收到第一个 `run` 命令之前 → 落不进任何 tick 窗口。
  所以 `equiv_build.py --mark` 会再拉一个子进程（`--mark-probe-worker`，与 `probe_006.py --worker`
  同一套做法；必须另起进程，宿主的 VEH 异常时会 `TerminateProcess`），直接读宿主的 detour 记录。
- **标记里为什么无条件调 `WriteFile`/`CloseHandle`**：宿主的 `CreateFileA` 桩恒返回
  `INVALID_HANDLE_VALUE`，只有照样调下去，宿主才能记下「要写的那 47 字节」；游戏里拿到的是有效句柄，
  就是一次普通追加写。
- **三个 FILE 函数用裸名 extern 而非 `dllimport`**：见结论 4 的装配器 bug。

## 判据输出（真实跑过的）

### 1. 地址提取（040 / 006）

```
$ python simplexc/tools/gen_addrs.py --src work/hook/hook_src_040.si -o work/tmp/equiv/addrs.h
#define A_COMMANDS              0x210AEE20000ULL   /* commands */
#define A_SETTINGS              0x210AF140000ULL   /* settings */
#define A_INPUT_REPLAY          0x210AF150000ULL   /* input_replay */
#define A_OUTPUT_HISTORY_PINS   0x210AF160000ULL   /* output_history_pins */
#define A_ERROR_BUFFER          0x210AF170000ULL   /* error_buffer */
#define A_UI_BUFFER             0x210B2790000ULL   /* ui_buffer */
#define A_CTL_RESET             0x210AF190000ULL   /* ctl_input_replay_reset */
#define A_SIM_STATE             0x210B1DC0000ULL   /* #SIMULATION_STATE */
#define A_KB_CHAR               0x210B2990030ULL   /* #SIMULATION_KEYBOARD_CHARACTER */
#define A_KB_COORD              0x210B2993030ULL   /* #SIMULATION_KEYBOARD_COORDINATE */
#define A_KB_PUSH_BUTTON        0x210B2996030ULL   /* #SIMULATION_PUSH_BUTTON */
```
11 条逐字核对（脚本比 `源码十进制 → 头里十六进制`）：040 全一致、006 全一致。
006 那套是 `0x232A6FF0000/0x232A7000000/0x232A7010000/0x232A72A0000/0x232AA920000/0x232AA930000/
0x232AABC0000/0x232A9F50000/0x232AAB30030/0x232AAB33030/0x232AAB36030`（`work/tmp/equiv/addrs_006.h`）。

### 2. 宿主自比对（参考 vs 参考）：**原样跑不了，用替代判据**

```
$ python simplexc/tools/host/selftest.py --src work/hook/hook_src_040.si --bin work/hook/hook_out_040.bin --ticks 10
  --- stderr_a ---
hook_out_040.bin 在 manifest 里找不到
```
原因：`host.py` 的 `--entry` 缺省只查 `simplexc/corpus/manifest.tsv`（按产物名 stem 匹配），捕获产物不在那张表；
`selftest.py` 又没有 `--entry` 透传。替代判据 = 同一个 `selftest.py`，只给两条宿主命令补 `--entry`
（`work/tmp/equiv/ref_selfcheck.py`，把 `selftest.run_host` 包一层，其余全复用）：

```
$ python work/tmp/equiv/ref_selfcheck.py --src work/hook/hook_src_040.si --bin work/hook/hook_out_040.bin --entry 0x212F --ticks 10
自比对 失败：tick 10/10（要 10）· 逐 tick 全等=是 · 仿真真在工作=否（工作页 0 个）· extern 0 条
  未过：sim_made_work
  未过：not_vacuous
```
逐 tick 全等通过了；两条防伪检查**因电路属性**失败：两边唯一变化的页就是 `0x210aee20000`（commands）与
`0x210af140000`（settings），而 `#SIMULATION_STATE`（`0x210b1dc0000`）那边写入的全是 0 → 页不变。
不是宿主缺陷，也不是空跑（`sim_cycle` 逐 tick 走到 1..10，见 §核心判据的 JSONL）。

### 3. 核心判据

```
$ python simplexc/tools/equiv_build.py --src work/hook/hook_src_040.si --bin work/hook/hook_out_040.bin \
      --entry 0x212F --workdir work/tmp/equiv --mark
[equiv] 参考产物 work\hook\hook_out_040.bin entry=0x212F sha256=6f0bbf6582e4bdef…（--entry）
[equiv] 地址头：work\tmp\equiv\addrs.h（11 个地址）
[equiv] 编译：work\tmp\equiv\equiv_cc_and.o
[equiv] 装配：work\tmp\equiv\ours.bin sha256=b61342fff8098264e1b39dc9285f0403fc26eddf457278f91ce30bff9a5af127 entry=0x620 extern 跳板 6 处
[equiv] .entry = 0x620
[equiv] .stubs:
    offset	kind	symbol
    8f0	stub	Sleep
    900	stub	GetSystemTimePreciseAsFileTime
    90c	slot	__imp_ExitThread
    914	slot	__imp_CreateFileA
    91c	slot	__imp_WriteFile
    924	slot	__imp_CloseHandle
[equiv] 逐 tick：10/10 tick（要 10），差异 0 条，白名单 0 条 → 全等
[equiv] 编译：work\tmp\equiv\equiv_cc_and_marked.o
[equiv] 标记版：work\tmp\equiv\ours_marked.bin sha256=3e776d1485c38687… 宿主 2 tick rc=0（驱动协议正常）；启动阶段 extern 记录 ['CreateFileA', 'WriteFile', 'CloseHandle']
[equiv] 标记内容：WriteFile 写了 47 字节，头 8 字节 = b'equiv_cc'
[equiv] 标记路径 work\tmp\equiv\equiv_cc_and.log 存在=否（宿主的 CreateFileA 桩恒返回 INVALID_HANDLE_VALUE → 不会真落盘；游戏内会写出真正的一行）
[equiv] 静态：参考 3046 条指令 / 12867 B（机器码 12859 B）；我们 541 条 / 2356 B（机器码 2348 B）
[equiv] 结果 → work\tmp\equiv\equiv_result.json；结论：通过
RC=0
```
两次独立构建（不同 `--workdir`、一次带 `--entry` 一次不带）产出的 blob sha256 相同 → 装配确定。

### 4. 标记路径在宿主里「通」的证据

`work/tmp/equiv/*.log` **不会**出现 —— 宿主的 `CreateFileA` 桩恒返回 `INVALID_HANDLE_VALUE`（它刻意不碰真磁盘）。
替代观察：宿主的 detour 记录（`--mark-probe-worker`），并与无标记版对照：

```
$ python simplexc/tools/equiv_build.py --mark-probe-worker --src work/hook/hook_src_040.si --bin work/tmp/equiv/ours.bin --entry 0x620
{"entry": 1568, "records": [], "alive": true, "close": "exited_clean", "pacing": {"Sleep": 2282162}}
$ python simplexc/tools/equiv_build.py --mark-probe-worker --src work/hook/hook_src_040.si --bin work/tmp/equiv/ours_marked.bin --entry 0x7a0
{"entry": 1952, "records": [{"name": "CreateFileA", "args": [], "ret": 18446744073709551615},
 {"name": "WriteFile", "args": [0, 47, 7161672794629304677], "ret": 47},
 {"name": "CloseHandle", "args": [18446744073709551615], "ret": 1}], "alive": true, "close": "exited_clean", "pacing": {"Sleep": 1815}}
```
`7161672794629304677` 小端 = `b'equiv_cc'`，47 字节 = `"equiv_cc_and tc_main, sim_state=2270726717440\r\n"`。
另外：标记文本与标记路径**只出现在标记版**的 blob 里（无标记版搜不到 `equiv_cc_and.log`）→ 默认确实没编进去。

## 未解决 / 存疑

- 装配器 `__imp_` 槽地址 bug（结论 4）—— 不在允许改的文件里，只加了守卫 + 绕法。**建议单独派活修**。
- `selftest.py` 没有 `--entry` 透传 → 捕获产物（`work/hook/hook_out_*.bin`）跑不了官方自比对。
  要么给 `selftest.py` 加一个 `--entry`，要么让 `host.py` 支持「在产物同目录找 manifest」。
- 这份电路的元件级状态全 0，宿主/游戏都看不出与门在算什么。要量收益或做游戏内 A/B，得挑一份
  `input_replay` 真会被写的电路（例如带 level 端口的那类）。
- 带标记的产物在游戏里还没跑过（本任务不部署）；`equiv_cc_and.log` 是否真写出来要等游戏内那一枪。
- 传输路径没验证：`addrs.h` 在 `work/tmp/equiv/` 下，`equiv_cc_and.c` 靠 `-I <workdir>` 找到它；
  单文件用 clangd/pi-lens 打开时（没有 `-I`）会报「找不到 addrs.h」，那是预期的（真判据是 clang 编译）。

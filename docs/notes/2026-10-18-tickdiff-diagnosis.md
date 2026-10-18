# TickDiff(40) 与 HostRc(20) 诊断：`in_scope` 缺口 与 宿主 VirtualAlloc 桩

生成 2026-10-18 · 只读产品代码，未改 `simplexc/**` 一个字节 · 诊断脚本在 `work/tmp/tickdiff/`

## 0. 结论

| 块 | 份数 | 根因 | 归因 |
| --- | --- | --- | --- |
| TickDiff | 40 | 我们的 lowering 把 `if in_scope("get_input"/"check_output")` 整块丢掉（判定只用**顶层 def 表**，而这两条 def 是 `run_sim` 里的**嵌套 def**） | **我们 lowering 的错** |
| HostRc | 20 | 参考产物启动时 `VirtualAlloc(NULL, 10^10, MEM_RESERVE, PAGE_NOACCESS)` ×N；宿主的桩把 RESERVE 当 COMMIT 真去提交 → 第 5 个起返回 0 → 参考产物拿 NULL 写 seq 头 → 写 AV | **宿主侧限制**（今天的口径下**判定不了**） |

60 份的归因：**40 份是我们 lowering 的错，20 份是宿主侧的限制（不是我们的差异）**。

## 1. A：40 份 TickDiff 是同一类缺口

### 1.1 分叉形状（逐份，`work/tmp/m5/logs/<id>.{ref,ours}.jsonl`）

40 份的**第一分叉全都**是「段哈希不同」，且差异段永远落在同一组 5 个段键里
（`0x232a6ff0000` = commands+settings+input_replay、`0x232a72a0000` = output_history_pins、
`0x232a9f50000` = `#SIMULATION_STATE`、`0x232aa920000` = error_buffer）。按差异段集合分桶：

| 差异段集合 | 有页差异 | 份数 | 语料 |
| --- | --- | --- | --- |
| 6ff0000 + 72a0000 + 9f50000 | 是 | 13 | 008 010 012 014 016 018 022 030 032 036 040 056 067 |
| 6ff0000 + 9f50000 + aa920000 | 是 | 12 | 042 063 065 082 094 124 126 128 129 137 153 155 |
| 6ff0000 + 9f50000 | 是 | 6 | 005 071 131 139 141 143 |
| 6ff0000 + 72a0000 + 9f50000 + aa920000 | 是 | 3 | 028 073 090 |
| 9f50000 (+aa920000) | 是 | 4 | 104 106 110 112 |
| 6ff0000 (+aa920000) | 是 | 2 | 050 120 |

`9f50000`（`#SIMULATION_STATE`）出现 38/40 —— 它是 `mode_refresh` 每 tick 从 `input_replay[0]` 抄出来的镜像，
所以它一变，下游的 output_history_pins/error_buffer 跟着变。分叉点**都在同一条依赖链上**。

### 1.2 三份深挖：字节级差异（同一形状）

`python work/tmp/tickdiff/probe.py <id> ref|ours 1 --dump-regions work/tmp/tickdiff/<id>/reg.<side>`，
再用 `python work/tmp/tickdiff/diffreg.py <id> 1 <refdir> <oursdir>`（tick 1，灌 `--seed-reset-data seq`）：

```text
005 t1 0x232A7010000 (+0x20000) ref=0x01 ours=0x00   [input_replay[0]]
005 t1 0x232A9F50100            ref=0x01 ours=0x00   [SIMULATION_STATE+0x100]
005 t1 0x232A9F50101            ref=0x01 ours=0x00   [SIMULATION_STATE+0x101]

008 t1 0x232A7000010            ref=0x01 ours=0x00   [settings+0x10 = sim_test_result: win(1) vs pass(0)]
008 t1 0x232A7010000            ref=0x01 ours=0x00   [input_replay[0]]
008 t1 0x232A72A000A            ref=0x00 ours=0x01   [output_history_pins+0xA（下游）]
008 t1 0x232A9F50100/101        ref=0x01 ours=0x00   [SIMULATION_STATE+0x100/101]
008 t1 0x232A9F50102/103        ref=0x00 ours=0x01   [SIMULATION_STATE+0x102/103（~(0&0) 的下游）]

010 t1 0x232A7010000            ref=0x01 ours=0x00   [input_replay[0]]
010 t1 0x232A9F50100/101        ref=0x01 ours=0x00   [SIMULATION_STATE+0x100/101]
```text

对应源码（三份同形）：

```text
// 005.si:374    （在 `run_sim: while true {` 里，缩进 4 格 = 嵌套 def）
    def get_input(cycle: Int) Input { return Input {input: U1 1} }
// 005.si:281-291
            if in_scope("get_input") { level_input = get_input(cycle + 1) }   // ← 我们整块丢掉
            input_replay[0] = U64 level_input.input                            // ← 于是恒为 0
// 005.si:203-205 mode_refresh
    store(#SIMULATION_STATE + 256/257, U1 input_replay[0])                     // ← 于是恒 0
```text

- 008/010 的 `SIM_STATE+258/259` 是 008.si `mode_refresh` 的 `~(load+256 & load+256)`：ref 的 256=1 → 0，
  我们的 256=0 → 1，**同一原因的下游**，不是第二个缺口。
- 008 的 `settings+0x10` = `sim_test_result`：ref=win(1) 来自 `if in_scope("check_output") { handle_test_result(...) }`，
  我们同样整块丢掉 → 停在 pass(0)。**一份语料里两个被丢的块互相印证**。

### 1.3 根因（读代码，未改）

`simplexc/lower/lower.cc:963 in_scope_const()`：`return defs_.count(arg->text) != 0;`，注释写明
「**名字是顶层 def ⇒ 真，否则假**（U-L14 未定）」。而语料里 `get_input`/`check_output` 是
`run_sim` 内的**嵌套 def**（005/008/010 各有 2 条嵌套 def，顶层 def 39 条，名字不在 `defs_` 里），
于是 `in_scope("get_input") == false` → `s_IfStmt`（lower.cc:911）把整块丢掉。

参考侧的行为相反：ref 产物**执行了**这两块（否则 `input_replay[0]=1` 与 `sim_test_result=win`
都没有来源）。⇒ 40 份 TickDiff = **`in_scope` 真假判定错**（一处，不是 40 处）。

另外：005 ref 的 movabs 里 DLL 段 23 处（全是 kernel32 导入 thunk），我们的 0 处 —— 与本题无关，
但说明「ref 会自己申请内存」，正是 B 块的引子。

## 2. B：20 份 HostRc 的共性 + 可检验假设

### 2.1 共性（逐份原始证据）

- 崩溃**只有两个落点**：`产物 +0x6BC`（15 份：034 044 047 048 049 053 054 060 069 080 100 122 133 135 145）
  与 `产物 +0x1FBF`（5 份：001 051 061 086 088），两处都是 `code=0xC0000005 params=[1, 0]`（**写**，目标地址 ~0）。
- 两处机器码都是**同一条生成模式**：`48 89 08` = `mov [rax], rcx`，前一条是
  `b9 10 00 00 00 / 48 d3 e6 / 48 09 f9`（`(x << 16) | cap`）—— 即**写 Nim seq 头**（`p-8`/`p+8`），
  目标指针是 0。
- 20 份都是「参考产物很大」的那一档（ref_bytes 21931…458276），且**都**含 ≥2 个 `0x2540BE400`（10^10）立即数：
  001:8 034:4 044:5 047:8 048:8 049:6 051:8 053:8 054:8 060:7 061:7 069:5 080:20 086:7 088:7 100:9 122:4 133:2 135:2 145:34。
  40 份 TickDiff 里只有 22 份 ≥2（含 1 份组的 18 份为 ≤1）—— 这条只是**相关**，不是充分条件。
- **我们的产物同样跑得起来**：`work/tmp/tickdiff/probe.py 034|001 ours 3` → tick 1..3 全 `ok`。

### 2.2 机制（已复现，不是猜）

把宿主自己的 VirtualAlloc 桩包一层打印（`_virtual_alloc` 原样调用，只记 args/ret），跑 `034 ref`：

```text
VirtualAlloc(addr=0x0, size=0x2540BE400, type=0x2000, prot=0x1) -> 0x21BF3D60000   ← MEM_RESERVE|PAGE_NOACCESS
VirtualAlloc(addr=0x0, size=0x2540BE400, type=0x2000, prot=0x1) -> 0x21E47E20000
VirtualAlloc(addr=0x0, size=0x2540BE400, type=0x2000, prot=0x1) -> 0x2209BEE0000
VirtualAlloc(addr=0x0, size=0x2540BE400, type=0x2000, prot=0x1) -> 0x222EFFA0000
VirtualAlloc(addr=0x0, size=0x2540BE400, type=0x2000, prot=0x1) -> 0x0            ← 失败（第 5 次）
VirtualAlloc(addr=0x0, size=0x2540BE400, type=0x2000, prot=0x1) -> 0x0            ← 失败（第 6 次）
VirtualAlloc(addr=0x0, size=0x4000,      type=0x1000, prot=0x4) -> 0x100000000     ← 走宿主 arena
[host] 异常 code=0xC0000005 addr=0x21BF3D106BC（产物 +0x6BC） params=[1, 0]
```text

宿主 `Detours._virtual_alloc`（host.py:371）对 `addr == 0` 一律走 `_bump(size)`：
size 超 arena 时退化成 `k32.VirtualAlloc(None, size, MEM_COMMIT|MEM_RESERVE, PAGE_RW)` ——
**把 10 GB 的 RESERVE 变成真的 10 GB COMMIT**。提交到第 5 个（≈40 GB）时失败返回 0，
参考产物不检查返回值，直接拿它当 seq 数据指针写头 → 写 AV。

交叉验证：把 IAT 槽换成「原样透传真实 API 的桩」（`work/tmp/tickdiff/probe_va.py 034 ref 3`）→
14 次请求全部成功、**连跑 3 tick 不崩**（两次都如此）；走宿主桩/裸跑则 **2/2 崩**。
⇒ 崩不崩由**宿主的桩语义**决定，与产品代码无关；「同一命令重跑时好时坏」= 提交额度随机器内存压力变。

### 2.3 可检验假设（怎么验、预期看到什么）

假设 B：**这 20 份不可判，原因是宿主 `_virtual_alloc` 把大块 RESERVE 当 COMMIT 真提交，失败即返回 0，
而参考产物不检查返回值。**

- 验 1（已经验过，最小）：`python work/tmp/tickdiff/probe_va.py 034 ref 3` → 预期 3 tick 全 ok；
  `python simplexc/tools/host/host.py --src simplexc/corpus/src/034.si --bin simplexc/corpus/ref/034.bin --ticks 10`
  → 预期 `产物 +0x6BC` 写 AV。两条命令的差别只有那一个桩。
- 验 2（日志）：包一层 `_virtual_alloc` 打印（§2.2）→ 预期在第 5 个 `size=0x2540BE400` 处看到 `-> 0x0`，
  紧接着 AV；把 VM 提交额度调大/换台空闲机器 → 失败点后移、崩的份数减少（**预测可证伪**）。
- 验 3（外推）：改成「`typ` 只含 MEM_RESERVE(0x2000) 时透传真实 API，不提交」→ 预期这 20 份不再崩，
  然后它们**大概率落进 A 的同一类缺口**（001/034/133/145 的 `get_input`/`check_output` 同样是
  `run_sim` 里的嵌套 def，`if in_scope("get_input")` 同样是裸 if）。

### 2.4 对 M5 验收口径的建议

- 20 份在**当前**口径下写「不可判（宿主侧 refAV）」，不进「我们的差异」计数；口径文字要带
  「ref 崩在 `产物 +0x6BC/+0x1FBF` 的 seq 头写，原因是宿主 VirtualAlloc 桩」+ 上面的验 2 命令。
- 顺带修掉住宿主的桩之后，这 20 份才回到可判；预期那时它们的差分与 A 同因。

## 3. 复现命令（都是只读）

```bash
# A：逐份分叉（两边都跑，10 tick）
python simplexc/tools/host/host.py --src simplexc/corpus/src/005.si --bin simplexc/corpus/ref/005.bin --entry 0x212f --ticks 10 --seed-reset-data seq --out work/tmp/tickdiff/005.ref.jsonl --quiet
python work/tmp/tickdiff/probe.py 005 ref 1 --dump-regions work/tmp/tickdiff/005/reg.ref
python work/tmp/tickdiff/diffreg.py 005 1 work/tmp/tickdiff/005/reg.ref work/tmp/tickdiff/005/reg.ours
# B：宿主的 VirtualAlloc 桩
python work/tmp/tickdiff/probe_va.py 034 ref 3          # 透传真实 API → 不崩
```text

## 4. 遗留风险

- 40 份只**逐字节**验了 005/008/010 三份；其余 37 份是按「差异段集合（§1.1）+ 源码同形（嵌套 def +
  裸 `if in_scope(...)`）」外推的。要收口就跑一遍 `in_scope` 修正后的产物差分（判据已有）。
- `in_scope` 的正确规则（U-L14）本报告只给到「参考侧在这 40 份里判真」；是否**所有**嵌套 def 都该判真、
  以及旧的 `#if in_scope(...)`（003/006/040 那批）为什么判假，需要主程按 L14 定稿。
- B 的机制在本机（提交额度 ~40 GB 时失败）成立；别的机器上失败点不同，所以结论写成「宿主桩语义」而不是「第 5 个」。
- 20 份里 133/135 只含 2 个 10^10 立即数，与本机 6 次调用的观察不一致（立即数个数 ≠ 调用次数），
  所以「≥2 个立即数」只能当相关线索，不能当判据。

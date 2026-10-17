# 差分宿主加「`#RESET_DATA_*` 灌确定性图案」—— 272 MB RAM 镜像第一次进判据（并立刻炸出一个差异）

**一句话**：`host.py --seed-reset-data {seq,xorshift}` 在跑第一个 tick 前把 `#RESET_DATA_*` 的实际区间
（rv32im：4 块 / 136.02 MB）填成确定性图案，图案再由程序自己的 `reset_sim` 拷进 `#DATA_*`。
灌了之后 rv32im 的「逐 tick 全等」（`docs/facts.md` §M3 达成）**不再成立**：tick 1 起就来 8 条差异，
根因是**我们 lowering 的 `memory_copy` 源/目的反了** —— 灌图案前它是隐形的（两边都全零，拷零等于没拷）。

## 1. 开关与图案

| 参数 | 值 | 定义 |
| --- | --- | --- |
| `host.py --seed-reset-data` / `selftest.py --seed-reset-data` | `seq` | `byte[i] = i & 0xFF`（每 256 字节一循环） |
| 同上 | `xorshift` | xorshift64（13/7/17），种子 `0x2545F4914F6CDD1D`，每步取 8 字节小端 |
| 不传 | — | **不灌**，保持全零的历史行为 |

- 图案是**块内偏移**的函数：每个块都从同一个种子重新起 → 加/减一个块不改变别的块的字节。
- 合法值只在一处定义（`host.py` 的 `SEED_PATTERNS`，`argparse choices` 用它；见 `docs/pitfalls.md`）。
- `selftest.py` 把值**透传给两条宿主命令**，报告 JSON 多一个 `seed_reset_data` 字段、打印行多一段
  `#RESET_DATA 图案=…`。⚠️ **两边同一个值 = 这条判据成立的前提**：图案不同的话段哈希与 `pages`
  必报差异（那不算合法判据）。两个独立进程用同一个图案是**确定**的 —— 证：
  `selftest.py --src …/rv32im.si --bin …/rv32im.bin --seed-reset-data seq --ticks 10`
  → `自比对 通过：逐 tick 全等=是 · 段哈希 6 段/10 tick=有 · 仿真真在工作=是（工作页 2181 个）`
  （不灌图案时工作页只有 **1** 个）。

## 2. 灌哪几块 —— 区间按 `memory_copy` 的**源实参**算，不是 `c_alloc(N)`

rv32im 灌下去的是 4 块 / **136.02 MB**：

| 名字 | 灌的区间 | 长度 | 记录地址 |
| --- | --- | --- | --- |
| `RESET_DATA_7069020504170261741` | `[0x18920150000, 0x18928151000)` | 134221824 B（128 MB + 4096） | `0x18920150800` |
| `RESET_DATA_4620396928043218538` | `[0x189111C0000, 0x189111C1080)` | 4224 B | `0x189111C0800` |
| `RESET_DATA_7585293127307814633` | `[0x18928970000, 0x18929171000)` | 8392704 B（8 MB + 4096） | `0x18928970800` |
| `RESET_DATA_1757052846952780449` | `[0x18911200000, 0x18911201400)` | 5120 B | `0x18911200800` |

**第一版按 `c_alloc(N)` 的 N 字节灌，错得很安静**：声明块的块首比程序用的记录地址低 2048，
而记录窗口是 `[记录, 记录+N)` = `[块首+2048, 块首+2048+N)`，两者只重叠前 `N−2048` 字节 ——
`4620` 组（`c_alloc(128)`）与 `1757` 组（`c_alloc(1024)`）**一字节都重叠不到**，程序读到的仍是零。
探针（§4）第一版里这两组 tick 1 后全是 `00 00 …` 就是这么来的。改成「`memory_copy` 源实参给出的
`[块首, 块首+n)`」后，`n`（rv32im 的 4 个组）都比 N 多 4096，完整盖住记录窗口。

落地位置：

- `simspec.py`：解析 `memory_copy` 时记下源侧（`self.reset_copies`）→ `reset_data_blocks`
  给出「实际区间」（没有 `memory_copy` 引用的 `#RESET_DATA_*` 才退化成 `c_alloc(N)` 的 N 字节）；
  另加 `named_addrs()` 供下面的硬检查用。
- `host.py`：`_seed_reset_data()` 在 `_seed_memory()` 之后、`_snapshot_baseline()` **之前**跑
  （基线带上图案 → `pages` 仍是「程序相对 pre-run 基线改动了什么」，语义没变），
  两条**硬检查**：① 区间必须在已映射区间内；② 灌的那段里不许出现**任何别的已声明对象**
  （盖住就报错退出）—— 这两条只加强，没放宽任何既有判据。
- 只碰 `#RESET_DATA_*`；`#DATA_*` 由程序自己的 `reset_sim` 灌（那正是要验的路径），
  `commands`/`settings`/`#SIMULATION_STATE`/`error_buffer` 一概不碰。

顺带（为以后选靶子用）：语料 99 份里 **28 份**带 `#RESET_DATA_*`，除 rv32im 外还有
`180.si`（136.02 MB）、`179.si` / `178.si`（64 MB）、13 份 16 MB 的。

## 3. 灌图案前 / 后段哈希对照（rv32im tick 1，按源码算的 6 段）

| 段 | 大小 | 全零 sha256 | 不灌（A/B） | `seq`（A/B） | `xorshift`（A/B） |
| --- | --- | --- | --- | --- | --- |
| `0x188b5190000` | 1.250 MB | `439292e4…` | A=B `1302fb70…` ≠0 | A=B `1302fb70…` | A=B `1302fb70…` |
| `0x188b7cd0000` | 1.000 MB | `30e14955…` | A=B `25441790…` ≠0 | A `7fcd58cd…` / B `25441790…` | A `9aa57d45…` / B `25441790…` |
| `0x188b86a0000` | 1.000 MB | `30e14955…` | A=B `30e14955…` **=0** | A=B `30e14955…` =0 | A=B `30e14955…` =0 |
| `0x188b88a0000` | 1.562 MB | `0655c923…` | A=B `f82be5f6…` ≠0 | A=B `f82be5f6…` | A=B `f82be5f6…` |
| `0x189111a0000` | 1.438 MB | `58c83901…` | A=B `58c83901…` **=0** | A `6165fb5d…` ≠0 / **B `58c83901…` =0** | A `3e6dae7f…` ≠0 / **B `58c83901…` =0** |
| `0x18918140000` | **272.250 MB** | `cbcb79a5…` | A=B `cbcb79a5…` **=0** | A `74651a78…` ≠0 / **B `cbcb79a5…` =0** | A `f6a570a2…` ≠0 / **B `cbcb79a5…` =0** |

- 「不灌」那一列就是 `docs/facts.md` 记的边界：272.25 MB 的 RAM 镜像段哈希 == 同长度全零缓冲的 sha256
  （本次在最终代码态重算过，逐位相同）。
- 灌了图案之后，**参考产物侧**那两段（`0x189111a0000`、`0x18918140000`）不再等于全零哈希 → 图案真的
  进去了（这条是任务要求证的第 2 条）。
- **ours 侧没变**（还是全零哈希）→ 图案被我们自己的产物**抹掉了**，这正是下面的差异根因。

复算：`python work/tmp/seed/zero_check.py`（输出 `work/tmp/seed/zero_check.txt`）。

## 4. rv32im 灌图案后的主判据：**有差异**（tick 1 起，8 条，seq/xorshift 同形）

命令（同一口径 = `selftest.compare`，两边同一份 `--src`）：

```text
python work/tmp/seed/run_pair.py --tag seedseq2 --host-extra "--seed-reset-data seq"
python work/tmp/seed/run_pair.py --tag seedxor  --host-extra "--seed-reset-data xorshift"
# A = simplexc/corpus/rv32im/rv32im.bin (entry 0x215B)；B = work/tmp/tern/rv32im.bin (entry 0xE1F0)
```

```text
[pair] 差异 8 条（口径 = selftest.compare）
  tick  1: reason ok/ok sim_cycle 1/1 页 2183/2183 页不同 A独有 2181 B独有 2181 extern 0/0
          段 0x188b7cd0000: A=7fcd58cd81170479 B=2544179080603279
          段 0x189111a0000: A=6165fb5dae43f86f B=58c839011ef2f755
          段 0x18918140000: A=74651a780189284e B=cbcb79a53e082209
          A 独有页区间 [('0x188b7cd0000',…,1), ('0x189111b0000',…,1), ('0x189111f0000',…,1),
                        ('0x18918140000','0x18920140000',2049), ('0x18928160000','0x18928960000',129)]
          B 独有页区间 [('0x188b7cd0000',…,1), ('0x189111c0000',…,1), ('0x18911200000',…,1),
                        ('0x18920150000','0x18928150000',2049), ('0x18928970000','0x18929170000',129)]
（tick 2..10 同形；`sim_cycle` 1..10 两边都对，没有提前 halt；extern 两边都是 0 条）
```

**第一处差异**：`tick 1`，段 `0x18918140000`（272.25 MB RAM 镜像，A `74651a78…` / B `cbcb79a5…`）
与段 `0x189111a0000`（小的 `#DATA_*`/`#RESET_DATA_*`/`#SETTINGS_*`，A `6165fb5d…` / B `58c83901…`）
—— B 侧的这两个值**恰好是全零哈希**，另加 `#SIMULATION_STATE` 段（A `7fcd58cd…` / B `25441790…`
= 不灌图案时那个值）。也就是说：**灌图案后 ours 的内存状态与「从不灌图案」时逐段相同**。

页级明细（tick 1，`seedseq2`）：A 变的是 `#DATA_*` 那几页（`0x189111b0000` `0x189111f0000`
`0x18918140000..0x18920140000` `0x18928160000..0x18928960000`），B 变的正好是**镜像初值**那几页
（`0x189111c0000` `0x18911200000` `0x18920150000..0x18928150000` `0x18928970000..0x18929170000`）。

### 定位：探针直接看「图案被搬到哪去了」

```text
python work/tmp/seed/probe_mirror.py --bin simplexc/corpus/rv32im/rv32im.bin --entry 0x215B --pattern seq
python work/tmp/seed/probe_mirror.py --bin work/tmp/tern/rv32im.bin          --entry 0xE1F0 --pattern seq
```

| 组 | 参考产物 tick 1 后 | ours tick 1 后 |
| --- | --- | --- |
| `#DATA_7069…` | `00 01 02 …`（图案） | `00 00 00 …`（零） |
| `#RESET_DATA_7069…` | `00 01 02 …`（原样） | `00 00 00 …`（**被抹掉**） |
| `#DATA_4620…` / `#RESET_DATA_4620…` | 图案 / 原样 | 零 / 被抹掉 |
| `#DATA_7585…` / `#RESET_DATA_7585…` | 图案 / 原样 | 零 / 被抹掉 |
| `#DATA_1757…` / `#RESET_DATA_1757…` | 图案 / 原样 | 零 / 被抹掉 |

（两侧 pre-run 都是「`#DATA_*` 全零、`#RESET_DATA_*` = 图案」。）

### 根因（越界，只诊断没改）：`memory_copy` 的源/目的反了

`simplexc/lower/ir_build.py`：

- 调用点（`call_prefix`，第 1478 行附近）把 `node.args[0] / [1] / [2]` 原样传下去 —— 即源码的
  `memory_copy(源, 目的, n)`。生成 IR：`call void @memory_copy(i64 1688460394496, i64 1688326111232, 134221824)`
  （`1688460394496` = `#RESET_DATA_7069` 块首，`1688326111232` = `#DATA_7069` 块首）。
- 但实现 `_body_memory_copy`（第 1570 行附近）把 **param0 当目的**、param1 当源：
  `dst = inttoptr(GetParam(0))`、`src = inttoptr(GetParam(1))`。→ 实际执行的是「目的 ← 源」反过来。
- 第 1480 行的 `Unsupported("memory_copy(dst, src, n) 的形态")` 也写着同一个错约定。

三路独立证据：① 上面的 IR 调用点 + 参数绑定；② 运行时探针（图案从 `#RESET_DATA_*` 消失、
出现在 `#DATA_*` 的只有参考产物侧）；③ `docs/facts.md` 里参考产物的反汇编
（`+0x2391` 的 `rep movs`：`rsi = #RESET_DATA - 0x800`、`rdi = #DATA - 0x800` → **第一个实参是源**）。

为什么以前看不见：两边本来都全零，`memcpy` 拷零到哪边都没区别 —— 这正是「272 MB 全零」那条边界
盖住的东西。**修法（不在本任务范围内）**：把 `_body_memory_copy` 的两行
`GetParam(0)/GetParam(1)` 对调（或对调调用点两个实参），顺手改掉第 1480 行的错误约定说明；
改完 ours 的产物会变（`58612 B / entry 0xE1F0` 那套数字失效），M3 的「逐 tick 全等」要在灌图案的
口径下重跑一遍。

## 5. 耗时

| 动作 | 耗时 |
| --- | --- |
| 灌图案 `seq`（136.02 MB，4 块） | **0.08 s** |
| 灌图案 `xorshift`（同上） | **3.24 s**（纯 Python 逐 8 字节生成；只在传了开关时才付这个钱） |
| 参考产物 10 tick | 不灌 2.8 s → `seq` 4.5 s → `xorshift` 7.5 s（多出来的是灌 + 每 tick 多出 2180 个变化页的哈希） |
| `selftest.py`（两条宿主） | 不灌 5.6 s → `seq` 7.7 s |
| 段哈希（272 MB × 6 段/每 tick） | 没动，仍是每 tick 都算 |

## 6. 判据有没有被放宽：没有

- 页白名单、段哈希、地址表校验（`_reject_mapped_movabs`）、`UnsupportedReloc`、`--region-hash-every >= 1`
  一律没动；`selftest.py` 的 4 条防伪检查（tick 数 / 逐 tick 全等 / 工作页 / 段哈希字段在场）照旧，
  只多打印一行图案名。
- 新增的两条是**硬检查**（灌的区间必须在映射内、不许盖住别的已声明对象），方向是加强。
- `python simplexc/tools/check.py` → **4 步骤全 PASS**（corpus / host_selftest / hook_offline / probe_006）。
- 默认行为不变（第 1 条判据）：`python work/tmp/seed/judge_fields.py` → 006、rv32im 参考、rv32im ours
  三对的**判据字段**（`pages`/`regions`/`extern`/`sim_cycle`/`reason`/`stabilized`/`alive`/`sim_test_result`）
  改前改后**差异 0 处**；唯一变的字段是 `pacing`（`Sleep`/读时钟条数，随真实时间变，本来就不进判据，
  见 `docs/facts.md`）。参考产物 tick 1 的 6 个段哈希改前改后**逐字节相同**。

## 7. 还剩什么没覆盖

1. **`memory_copy` 方向这个 bug 本身没修**（`simplexc/lower/**` 不在允许改动的范围内）。修完要重跑
   本文件 §3/§4 两张表；届时「灌图案后差异 0 条」才算 M3 的新口径达成。
2. **差异的下游传导没分离**：`#SIMULATION_STATE` 段与页也跟着变（RAM 内容不同 → 被模拟的 RISC-V
   执行路径不同）。目前只能确认「第一处差异」在 RAM 镜像段，无法逐条断言哪些差异是拷贝方向单一
   根因、哪些是它传导出来的。
3. **游戏内仍全然未验**：图案是人为造的「确定性非零」，不是游戏里那份真 ROM/程序映像；
   `#RESET_DATA` 的真内容、真 Sleep/时钟、游戏设的 target_cycle 照旧没测。
4. **只有两种图案**，而且都不像「程序映像」：`seq` 每 256 字节一循环，`xorshift` 纯伪随机。
   真要演练取指/跳转，更值的是把一段可执行的 RV32IM 代码（或从参考产物/真 RAM 快照里摘一段）
   放进 RAM，但那需要新的图案来源，本次没做。
5. **灌图案不影响协议区这条只有结构保证**：靠「区间只来自 `#RESET_DATA_*` 的 `memory_copy` 源」+
   两条硬检查。没有构造过一个「故意让 RESET_DATA 区间盖住 `commands` 的假源码」来验硬检查会响
   （`work/tmp/seed/` 里可以补，本次没补）。
6. 只跑了 rv32im 与 006 两个靶子；另外 27 份带 `#RESET_DATA_*` 的语料没跑（`180.si` 与 rv32im 同量级，
   值得接着跑）。

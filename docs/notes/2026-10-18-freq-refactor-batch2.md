# freq 重构批 2（上层）：`sampler` / `report` / `driver` / `cli_watch` / `cli_bench`

**一句话**：采样、分段、报告、驱动四件事从旧脚本里抽进 `tools/freq/`，采样口径统一为
**median**；`watch_freq.py` / `bench_rewrite.py` 变薄壳（各 20 行），偏移副本、私有采样
循环、私有 `latest_product` 全部消失。离线重放历史数据：`freq_summary.txt` **逐字节一致**，
`bench_*.tsv` 除三处已批准差异外逐字节一致。

- 规格：`docs/specs/2026-10-15-freq-measurement-design.md` §3/§4/§5、§7 决策 3·5、§8 待办 3·4
- 判据脚本与原始输出：`work/tmp/freq1/`（`replay_bench.py` / `replay_watch.py` /
  `json_sample.py` / `preflight_check.py` / `verify_all.txt`），基线快照 `work/tmp/freq1/baseline/`
- 前置决策：输出契约在 §4 与「逐字符不变」之间冲突，负责人定为 **B**（保格式骨架 + 差异逐项归因）

## 1. 模块边界

| 模块 | 管什么 | 不管什么 |
| --- | --- | --- |
| `sampler.py` | `Sample`/`Segment`/`Region`、`Segmenter`、`Sampler` 采样循环（相位对齐 + 兜底间隔 + 丢首个）、`collect`、事件 `Sink`、停机/低压判据、`POLL`/`OPT_WAIT`/`SEG_TOL` | 不排版（`report`）、不驱动游戏（`driver`）、不读产物（`products`） |
| `report.py` | `pad`/`pad_left`/`disp_width`/`fmt_hz`/`bar`/`hz_style`、**区间表** `summarize`/`summary_text`/`print_summary`、**对照表** `bench_table`、`BenchRow`、`bench_tsv`/`write_bench_tsv`、`Report`/`VariantReport`/`build_report`（`schema=1`） | 不采样、不碰 `opt.ini` |
| `driver.py` | `simctl` 薄封装（`run`/`stop`/`recompile`/`restore`/`blocking_dialog`/`find_window`）+ `preflight()` 六条前置断言；`Result`/`Check` 都带可执行 `hint` | 不做编排（`cli_bench`）、不改 `simctl` 接口 |
| `cli_watch.py` | watch 的循环：编译点/游戏退出重连/心跳/总结 | 采样与排版都在上面两层 |
| `cli_bench.py` | 编排（换变体 → 重编译 → 运行 → 采样 → 停止）、`--variant`、`--json`、`opt.ini` 备份恢复 | 单步原语都在 `driver`/`sampler` |
| `tools/watch_freq.py`·`tools/bench_rewrite.py` | 各 ~20 行薄壳：把 `tc-toolkit/` 加进 `sys.path` 后转调 `tools.freq.cli_*` | —— |

## 2. sampler 的四条判据怎么落的

| 判据 | 落法 | 在哪 |
| --- | --- | --- |
| **相位对齐** | `Sampler.poll()` 只在 `last_speed_check` 变了（`fresh`）或到 `interval`（`due`）时出样本；watch 与 bench 共用同一条循环 | `sampler.Sampler.poll` |
| **丢首个** | `Sampler.drop` / `collect(drop=…)`；bench 传 1，watch 传 0——watch 靠分段把过渡段标出来、统计时不选它，丢首个会改它的历史输出 | `sampler.collect`、`cli_bench._bench_one` |
| **停机判据** | `hz == 0` 是游戏自己归零的**哨兵**（facts ①）；段以「0 与非 0 不混段」切分；`Segment.stopped`。**不用** `last_speed_check` 判在不在跑（facts ④，它停机时照常刷新），它只用于判「读数新不新」 | `sampler.is_stopped`、`Segmenter.add` |
| **低压判据** | `is_low(hz) = 0 < hz < LOW_HZ(1e6)`——电路自带限速停在**非 0 的极低值**，是真信号，**只标注不丢**；`report.hz_style` 复用同一个 `LOW_HZ` | `sampler.is_low`、`report.hz_style` |

口径 **median**：`Segment.median` / `BenchRow.median` / `build_report` 全部 `statistics.median`；
新代码里**没有** `statistics.mean`（`grep` 为空）。旧 bench 的 mean 口径按 §7 决策 3 废弃。

## 3. report 的两张表 + JSON

**区间表**（watch）：按「一次编译 → 一次运行」归并，每区间取**最大的非停机段中位数**当代表，
列 `# 主频 样本 波动 指纹 改写 rewrite`。与旧输出逐字符一致（见 §5）。

**对照表**（bench）：每个变体一行；列头 `平均 → 中位数`（决策 3 的口径变了，标签跟着改，
不让标签说谎）；数值取 median；相对基线 = `(med/base - 1)*100`，基线只认第一个变体。

**JSON**（`cli_bench --json`）：`Report.schema = 1`，顶层 `schema/when/baseline/variants`；
每个变体含 `compiler`/`rewrite`/`num`/`product_sha`/`impl`（**实际走了哪个实现**，来自
`hook.log` 的 `impl` 行）/`impl_config`/`samples`/`median_hz`/`spread_pct`/`relative_pct`/
`fallback_rate`/`fallback_reasons`。**回退率按变体算**（`ours*` 却跑成非 `ours` 的轮次 / 该变体
所有已知 `impl` 的轮次），`fallback_reasons` 按实际 `impl` 归类。样例见
`work/tmp/freq1/bench_sample.json`（三个变体：0.0 / 0.0 / 1.0）。

## 4. driver 的前置断言

`driver.preflight(expect_pid=None)` 返回 `Check` 列表，**不抛异常**；每条失败都带可执行 `hint`。

| # | 断言 | 离线表现（游戏没跑） |
| --- | --- | --- |
| 1 | 游戏在跑 | ✗ `Turing Complete.exe 没在运行` → 「先在 Steam 里把游戏启起来」 |
| 2 | PID 稳定 | 查不了（没 pid）→ 同上 |
| 3 | 偏移可读 | 查不了 → 同上 |
| 4 | 无阻塞弹窗 | 查不了 → 同上 |
| 5 | 能抢前台 | 查不了 → 同上 |
| 6 | 撤销栈非空 | 查不了 → 同上；**而且它本来就无法前置探测**（游戏不暴露栈深），标 `verifiable=False`，发键后由 `products.wait_new_product` 的失败解释指出来 |

游戏在跑时的判定：PID 与 `expect_pid` 比（进程变了 → 偏移全失效）、开句柄读 `sim_speed`、
`simctl.blocking_dialog`、`find_game_window` + `force_foreground`。

## 5. 离线重放逐字符对照

基线 `cp work/freq/ work/tmp/freq1/baseline/`（动手前）。重放脚本见 §开头。

### 5.1 `freq_summary.txt` —— **逐字节一致**

`replay_watch.py` 从 `freq_log.tsv` 解出区间/分段结构（`compile` 行的 `指纹=/…/rewrite=`
解出指纹/改写处数/模式），再喂新 `report.summarize` + `summary_text`：

| 重构口径 | 结果 |
| --- | --- |
| A 用段日志的 **1 位**波动 | 只差一格：`0.16 → 0.20`（段日志只记 1 位小数，2 位值取不回来） |
| B 用基线的 **2 位**波动 | **文本与字节都一致**（191 B，同为 CRLF） |

即：标题、`#`/主频/样本/指纹/改写/rewrite 六列都是从日志**独立**复现的；唯一靠基线补的是那
一格波动，因为 `freq_log.tsv` 的 `波动=X.X%` 只有 1 位小数。**这不是采样/聚合口径问题**
（`summarize` 的波动公式与旧版逐字符相同），是旧事件日志的粒度。

### 5.2 `bench_*.tsv` —— 除三处已批准差异外逐字节一致

`replay_bench.py` 把历史 TSV 的**原始 samples** 喂进新 `report.bench_tsv`：

| 类别 | 内容 | 是否 §2 要修的缺陷 |
| --- | --- | --- |
| (a) median 数值 | `off 5316338→5333878`、`on 6775380→6785375`、`short 8034268→8064682`、`cse 8028881→8038199` | **是**（§2 缺陷 3 / §7 决策 3，mean→median） |
| (b) 新增两列 | 末尾 `\t-\t-`（`compiler`/`impl`；历史无此字段，按决策 5 填 `-`） | 决策 5（已批准，非缺陷） |
| (c) 列头 | `fp      mean_hz         sample_n`（空格对齐）→ `fp\tmedian_hz\tsample_n\tcompiler\timpl` | 决策 3/5 |
| 其余 | `mode`/`num`/`fp`/`sample_n`/`note`/`**samples**` 逐字节一致 | —— |

**两个额外发现（都不是本批引入的）**：

1. **基线表头是空格对齐的，任何已提交版本都写不出来** —— 旧提交版 `bench_rewrite` 写的是
   `mode\tnum\tfp\tmean_hz\t…`（全 tab）。`replay_bench.py` 的 `[0]` 段用**旧提交版写法**对同一批
   samples 重写一遍，表头就与基线不同 → 说明基线表头是手工/旧稿对齐的产物，与本次重构无关。
2. **基线 `cse` 行的 `mean_hz` 与自己记的 samples 差 1**：按 samples 重算 mean = `8028882`，基线写
   `8028881`。原因是旧版把 **round 过的整数**写进 `samples` 列、却拿**原始 float** 算 mean。所以
   「旧版重算」本身就对不齐 → median 起了同样的作用（此处 median=8038199，与 mean 无关）。

## 6. 旧 CLI 薄壳化的差异

| 项 | 旧 | 新 | 说明 |
| --- | --- | --- | --- |
| `watch_freq.py --help` | —— | —— | **逐字符一致**（仅 prog 名随文件） |
| `bench_rewrite.py -h` | usage/description/epilog | 多 `--variant`/`--json`；description 加「变体（compiler × rewrite）」；`-s`/`-r` 帮助里「模式」→「变体」；epilog 多一行 `--variant` 示例 | §4 新增选项，格式骨架不变 |
| `bench_rewrite.py -n`（dry-run） | `off → …`、`[第1轮] rewrite=off → …` | `reference:off → …`、`[第1轮] reference:off → …` | 变体轴带出来的（`-m` 路径的 compiler 取自 opt.ini） |
| 参数集合 | `-m -s -d -r -t -n` | 同上 **全部保留**，只增 `--variant`/`--json` | 无回归 |
| `watch_freq.py:66-70` 偏移副本 | 一份 | **删除** | 偏移现在只有 `probe.py` 一处 |

## 7. 未验 / 留待

- **与游戏相连的部分未验**：全程游戏没在跑 —— `driver.run/stop/recompile/restore` 的真发键、
  `preflight` 游戏在跑时的五条判定、`sampler.collect` 的真内存读取、`cli_bench` 的真编排、
  `cli_watch` 的真采样循环**都没跑过**。验到的是：`preflight` 游戏没跑时的结构化返回、
  动作封装对 `simctl.Result` 的适配（用桩）、以及全部纯逻辑（采样循环/分段/表格/JSON/TSV）的单测。
- `opt.ini` 全程**只读**（md5 `3f371a59…` 未变）；`variants` 的写路径仍只在临时 ini 上验过。
- **human 对照表的 `模式` 列仍是 rewrite**（未加 `compiler` 列）：`--variant ours:off` 与
  `--variant reference:off` 两行在终端里同显示 `off`，靠 `产物`/`指纹` 区分；`compiler`/`impl`
  在 TSV 与 JSON 里。要保持表宽不变就先这么放 —— 若以后要并排看 compiler，得加一列（会动格式）。
- 本机没装 pytest，用 `unittest`（`python tools/freq/test_freq.py`，39 个用例全绿）。
- 旧 `bench_*.tsv` 的 `mean_hz` 与新 `median_hz` **不可直接比较**（口径不同；历史数字本来也不能
  跨模式比）。

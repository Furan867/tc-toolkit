# 设计：主频测量层（freq）—— 重写 watch_freq / bench_rewrite

- 日期：2026-10-15
- 分支：`main`（本设计尚未落代码）
- 状态：**架构定稿**（负责人 2026-10-15 确认 5 项决策，见 §8）；实现未开始
- 上游设计：`docs/specs/2026-10-15-simplex-llvm-compiler-design.md`（§7.3 游戏内 A/B、§7.5 判据入口）
- 证据等级：✅已证（读了代码/数据）｜🔍推断｜❓未确认

---

## 1. 目标与非目标

**目标**：给「A/B 主频对照」提供可复用的基础设施 —— 读频、驱动游戏、采样、聚合、报告；
供编译器项目的判据入口以**慢/手动步骤**调用；供人做长期监听。

**非目标**：
- 不改底层既有模块的对外接口：`winmem.py`（进程/内存读）、`term.py`（着色）、`hook_log.py`（日志契约）、
  `simctl.py`（驱动游戏）保持接口不变，本层只在其上分层。
- 不做自动检测（不监听文件 mtime 自动触发），不判断编译是否成功 —— 沿用 `simctl` 的三条设计原则。
- 不重写机器码改写器（`perf_opt/optimizer/core.py`）。

---

## 2. 现状与缺陷（✅ 均已读代码核实）

现在没有分层，是四份脚本各自成链：

| 层 | 实现 | 证据 |
|---|---|---|
| 读游戏内存 | `winmem.py`；偏移与读法**各写一份**：`read_freq.py:33-43`、`watch_freq.py:66-70,355-369` | ✅ |
| 采样循环 | **三份互不相同**：`read_freq.py:45-89`（独立轮询、无分段）；`watch_freq.py:385-565`（按游戏时间戳相位对齐 + 分段 + 重启重连）；`bench_rewrite.py:167-185`（固定 `sleep(1.0)` + 丢开头 1 个） | ✅ |
| 配置写入 | `bench_rewrite.py:81-107`（写 `opt.ini`，原子替换）；转发层读它：`compile_hook.c:340`；合法值唯一处：`perf_opt/optimizer/core.py:575-597` | ✅ |
| 权威记录 | `hook_log.py`（`work/opt/hook.log`，C 侧 `compile_hook.c:296` 同契约）；消费：`watch_freq.py:249-286` | ✅ |
| 产物身份 | `watch_freq.py:199-248` 与 `bench_rewrite.py:109-117` **两份语义不同的 `latest_product`**；`bench_rewrite.py:133-165` 的 `wait_new_product`（编号+mtime+hook.log 三判据） | ✅ |
| 聚合 | `watch_freq.py:287-354` 取**最大非停机段的中位数**；`bench_rewrite.py:259-307` 用**均值** + 相对**第一个模式**的百分比 | ✅ |
| 输出 | `Sink`（`watch_freq.py:116-137`）→ `work/freq/freq_log.tsv` + `freq_summary.txt`；`bench_rewrite.py:394-405` → `work/freq/bench_<ts>.tsv` | ✅ |
| 驱动游戏 | `simctl.py`（`run`=F6 切换、`stop`=F4、`recompile`=Ctrl+Z/Shift+Z 交替 + 净消耗记账、抢前台校验）；编排 `bench_rewrite.py:187-258` | ✅ |

缺陷：

1. **依赖方向反了**：`bench_rewrite.py:62 import watch_freq as W` —— CLI 模块被当共享库用，改 watch 的 CLI 会波及 bench。
2. **三处重复**：读频偏移三份；`latest_product` 两份语义不同；采样循环两份口径不同。
3. **口径不一致**：watch 用 `median`（`watch_freq.py:148,446`），bench 用 `mean`（`bench_rewrite.py:248,262,291,394`）
   → 同一次运行两条链给出不同数字。
4. **陈旧 docstring**：`watch_freq.py:20,214,233,423` 仍写"读 `optimize.log`"，而日志早已合并为单一 `hook.log`。
5. **`read_freq.py` 是重复实现**（无分段、无停机判据）—— 现在只值一个薄 CLI。
6. **零测试**：仓库里没有 `test_*.py`（✅）→ 采样/分段/归属语义只能靠人读总结回归。
7. **配置恢复靠 `atexit`**（`bench_rewrite.py:359-363`）：被 SIGKILL 会留下改过的 `opt.ini`（🔍，`simctl` 有 SIGTERM 处理，bench 没有）。
8. **"是否在跑"判据只做了一半**：watch 有相位对齐（`watch_freq.py:534`）与 `hz==0` 停机判据（`:541`）；
   bench 完全不判，靠 stop/run 编排 + 基线门槛兜（`bench_rewrite.py:262-265`）。

---

## 3. 建议架构

```
tools/freq/                 # 包（位置与命名已确认）
  probe.py     底层：偏移常量 + Snapshot(hz, game_time) + wait_for_game + 快照读取
  variants.py  变体定义 Variant(compiler, rewrite)；写/读回 opt.ini；合法性（唯一处）
  products.py  产物身份与归属：scan / latest / product_fp / opt_info / mode_from_hook_log / wait_new_product
  sampler.py   采样循环（相位对齐 / 丢首个 / 停机与低压判据）+ Segmenter + 事件 Sink
  report.py    pad / fmt / bar / hz_style + 两张表（区间表 / 对照表）+ JSON 报告（带 schema 版本）
  driver.py    驱动游戏的薄封装（run / stop / recompile / restore / blocking_dialog + 前置断言）
  cli_watch.py / cli_bench.py / cli_read.py
```

- **保留**：`winmem.py`、`term.py`、`hook_log.py`、`simctl.py`（作 `driver` 的底层，接口不变）。
- **移植**：`read_freq` 的偏移与自算；`watch_freq` 的 `read_hz`/`read_speed_time`/`Segmenter`/`Sink`/
  `scan_*`/`product_fp`/`opt_info`；`bench_rewrite` 的 `set_rewrite_ini`/`wait_new_product`/`bench_one`/`summarize`。
- **丢弃/降级**：`read_freq.py` 的独立采样循环（→ 薄 CLI）；bench 的私有采样与私有 `latest_product`；四处陈旧注释。
- **数据模型**：`dataclass` 的 `Snapshot`/`Sample`/`Segment`/`Region`/`Variant`/`Run`/`Report`；`Report.schema = 1`。
- **错误与超时**：游戏相关操作一律返回结构化 `Result`（沿用 `simctl.Result` 风格），不抛异常；
  每次等待必须带超时 + **可执行的失败解释**（沿用 `bench_rewrite.py:133-165` 的写法，那是现有代码里最好的一块）；
  `SIGINT`/`SIGTERM` 都要收尾（恢复 `opt.ini` + 出报告）。

---

## 4. 接口契约

- **变体轴**：`Variant(compiler ∈ {reference, ours, ours_with_fallback}, rewrite ∈ {off, on, short, short_cse})`。
  控制通道仍是 `opt.ini`（新增 `[opt] compiler=`，由转发层读；当前只有 `rewrite`，见 `compile_hook.c:340`）。
- **缺失键的 fail-safe 必须先定义**：`opt.ini` 读不到键时，`rewrite` 现按 `on` 兜底（`facts/perf-tooling.md`；
  实测当前生效值就是 `on`）；新增的 `compiler` 必须兜底到 **`reference`**，否则 A/B 会把“没配”误当“我们的实现”。
- **归属只认 `hook.log`，且要分清三行**：`cfg` = **配置了什么**（原样搬运）、`opt` = **改写器实际做了什么**、
  新增的 per-call `impl` 行 = **实际走了哪个实现**。不能用“我们写了什么配置”当归属
  （`facts/forwarding.md` §「产物 rewrite 模式的权威来源」已记该坑）；加 compiler 轴后，只记 `cfg` 会把回退误当我们的实现。
- **必须新增 per-call「实际用了哪个实现」行**（已确认；`impl` 词表与 manifest 列的定义在
  `2026-10-15-simplexc-interfaces.md` §5，那边是唯一一处）：回退是 per-call 的，只记 `cfg` 会重演
  "标注 vs 实际"错位（`compile_hook.c:337` 记的 `short_cse` 被静默改写成 `on` 那次就是同类问题）。
- **`hook.log` 移到 `work/hook/`**（已确认，名字更符合直觉）：
  - C 侧 `compile_hook.c:299`（`sprintf(path, "%s%chook.log", OPT_DIR, SEP)`）与 `:297` 的 `CreateDirectoryA`；
  - Python 侧 `tools/hook_log.py` 的 `PATH`；
  - 文档：`docs/map.md:18`、`docs/facts.md`、`compile_hook.c` 头部注释、`hook_log.py` docstring。
  - 消费者通过 `hook_log.PATH` / `hook_log.find()` 取路径，改这两处即可，不用逐个改调用方。
- **判据入口调用方式**：`cli_bench --variant reference:off --variant ours:off --json out.json`；
  判据入口**只消费 JSON**，人读摘要留在终端。
- **输出两层**：
  - 人读：保持现有表格形状与列（负责人对格式满意）；
  - 机器读：`bench_<ts>.json`（run → variant/compiler/rewrite/产物 sha/实际实现/samples/median/spread/相对基线 + schema）；
  - TSV 增列 `compiler`、`impl`，向后兼容。
- **回退率**：报告必须能按 per-call `impl` 行统计每个变体的回退率与回退原因分类
  （`ours` 与 `ours_with_fallback` 的差值就是回退的代价）。
- **前置断言（失败要吵）**：游戏在跑、PID 稳定、撤销栈非空、能抢到前台、无阻塞弹窗、偏移可读（与 buildid 绑定）。

---

## 5. 迁移路径

1. **抽库，零行为变化**：两个 CLI 的输出与旧实现逐字符对齐；**先把三份基线快照到 `work/tmp/baseline/`**
   （`freq_summary.txt` 每次覆盖写、`bench_*.tsv` 每轮新建、`freq_log.tsv` 追加），再用它们做回归判据。
2. **统一采样口径**：median + 丢首个 + 相位对齐 + 停机/低压判据；bench 改用统一 `sampler`。
3. **泛化变体轴**：`compiler` 维度先默认关，等转发层支持切换实现。
4. **加 JSON 报告 + schema**，接进判据入口（慢步骤）。
5. **收尾**：`read_freq` 降级为薄 CLI、修陈旧注释、补最小测试（分段 / 归属解析 / 失败解释）。

---

## 6. 风险（容易改坏的地方）

- **混合窗口**：丢掉"丢首个 + 中位数 + 过渡标注"会让数字静默变样。
- **停机哨兵 `0` vs 电路自带限速的低非零值**：把低非零当噪声丢，就丢了真信号（`facts/perf-tooling.md` §「采样判据四条」）。
- **不能用 `last_speed_check` 判断是否在跑**：停机时它照常刷新。
- **前台抢占**：不校验前台就发键，会把 Ctrl+Z 打到别的程序上。
- **undo 净消耗记账与栈空**：栈空时 `recompile` 静默无效，"`hook.log` 无新行"是唯一可靠探测。
- **采样抖动/温漂**：轮次在外交错；A/B 中途**不要重启游戏**（否则产物被清、指纹退化成 `-`）。

---

## 7. 已确认的决策（2026-10-15）

| # | 决策 |
|---|---|
| 1 | 包名与位置：`tools/freq/` |
| 2 | `read_freq.py` 保留，但降级为薄 CLI（偏移读取并入 `probe`） |
| 3 | 统一用 `median`（bench 的历史数字口径会变；旧数字本来也不能跨模式比较） |
| 4 | `hook.log` 新增 per-call「实际实现」行；同时把 `hook.log` 从 `work/opt/` 移到 `work/hook/` |
| 5 | TSV 增列 `compiler` / `impl`，向后兼容 |

---

## 8. 待办（实现任务拆解）

1. `tools/freq/` 骨架 + `probe`（偏移与快照），`read_freq` 改为薄 CLI。
2. `products`（合并两份 `latest_product`，保留 `wait_new_product` 的失败解释）+ `hook_log` 路径迁移 + per-call `impl` 行。
3. `sampler`（统一 median/丢首个/相位对齐/停机判据）+ `report`（人读表 + JSON schema 1）。
4. `driver`（薄封装 simctl + 前置断言）+ `cli_bench` 支持 `--variant`。
5. 回归：用现有三份数据逐字符比对；补最小测试；更新 `docs/map.md` / `docs/facts.md` / **`TOOLS.md`**
   （`TOOLS.md` 一直都在，见 `docs/map.md` §工具清单；工具的行为约定写那里，不是 `facts.md`）。

---

## 9. 证据索引

| 事实 | 复现方式 |
|---|---|
| bench 依赖 watch_freq | `tools/bench_rewrite.py:62` |
| median vs mean 口径不一致 | `watch_freq.py:148,446` vs `bench_rewrite.py:248,262,291,394` |
| `hook.log` 现有行类型 | `cut -f3 work/opt/hook.log \| sort \| uniq -c` → `cfg/entry/opt/writeback` |
| 日志契约（两个写入者） | `tools/hook_log.py` docstring + `compile_hook.c:296` |
| 权威配置来源只认 `hook.log` | `docs/facts/forwarding.md` §「产物 rewrite 模式的权威来源」 |
| 采样判据四条 | `docs/facts/perf-tooling.md` §「采样判据四条」 |

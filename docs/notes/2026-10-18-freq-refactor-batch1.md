# freq 重构批 1（数据层）：`probe` / `variants` / `products` + `read_freq` 降级为薄 CLI

**一句话**：读频偏移、变体（`opt.ini`）、产物归属三件事，从三份互不相同的脚本里合并进
`tools/freq/`，每个语义只剩一处实现；`tools/read_freq.py` 变成 21 行薄壳；`watch_freq.py` /
`bench_rewrite.py` 本批**原样不动**（下一批切过来），因此 `work/opt/hook.log` 迁移、`sampler`
等留给批 2。

- 规格：`docs/specs/2026-10-15-freq-measurement-design.md` §3（架构）/§4（契约）/§5（迁移）/§8（待办）
- 判据脚本与原始输出：`work/tmp/freq0/compare_products.py`（对照旧实现，离线数据）

## 1. 模块边界（本批落地的三个 + 一个入口）

| 模块 | 管什么 | 不管什么 |
| --- | --- | --- |
| `tools/freq/probe.py` | 偏移常量、`Snapshot(hz, game_time, cycle)`、`find_game` / `wait_for_game`、`snapshot` / `read_hz` / `read_speed_time`、`hz_from_cycle`（自算） | 不落盘、不打印（文案由调用方注入 `say`）、不 import `simctl` |
| `tools/freq/variants.py` | `Variant(compiler, rewrite)`、`COMPILERS`/`REWRITES`（合法值唯一一处）、`opt.ini` 读（`ini_values`/`read_variant`）与写（`set_variant`，原子替换） | 不备份、不恢复（那是 `driver`）；不碰 `opt.ini` 之外的配置 |
| `tools/freq/products.py` | `Product`/`latest_product`、`fingerprint`/`product_fp`、`scan_products`/`scan_captures`、`ownership`（cfg/opt/impl 三行）、`opt_info`/`mode_from_hook_log`、`wait_new_product`、`hook_log_path`/`hook_log_lines` | 不采样、不驱动游戏；`hook.log` 路径只从 `hook_log.PATH` 取 |
| `tools/freq/cli_read.py` | 人读的一次性入口（`--samples`/`--watch`/`--interval`），输出形状与旧 `read_freq.py` 一致 | 不做分段/相位/停机判据（批 2 的 `sampler`）；不产出机器可解析的报告 |

`tools/read_freq.py` = 21 行薄壳：把 `reversing/` 放进 `sys.path` 后调 `tools.freq.cli_read.main()`。
包内模块一律相对导入（`from .. import winmem`），所以 `py tools/read_freq.py` 与
`python -m tools.freq.cli_read` 两条路都通（`tools/` 没有 `__init__.py`，靠命名空间包）。

## 2. 偏移的唯一处

`tools/freq/probe.py:26-33` 是**唯一一处**把链接地址变成模块内偏移的地方：

```python
BASE_VA = 0x140000000
LINK_CYCLE = 0x14624D608      # last_cycle_count，int64
LINK_TIME = 0x14624D610       # last_speed_check，double
LINK_SIM_SPEED = 0x14624D618  # sim_speed__presenterZutilities_u29531，float32 Hz
OFF_CYCLE = LINK_CYCLE - BASE_VA
OFF_TIME = LINK_TIME - BASE_VA
OFF_SIM_SPEED = LINK_SIM_SPEED - BASE_VA
```

| 常量 | 旧 `read_freq.py`（git HEAD） | 旧 `watch_freq.py`（树内，本批冻结） | 新 `probe.py` |
| --- | --- | --- | --- |
| `OFF_CYCLE` | `0x14624D608 - BASE_VA` | 不读 | `0x624d608` |
| `OFF_TIME` | `0x14624D610 - BASE_VA` | `103077392` | `0x624d610` |
| `OFF_SIM_SPEED` | `0x14624D618 - BASE_VA` | `103077400` | `0x624d618` |

**三处逐项相同**。⚠️ `tools/watch_freq.py:66-70` 仍是第二处定义 —— 它本批被明令不许改
（要保持照旧能跑），批 2 切到新包时删除；`tools/bench_rewrite.py` 只在 docstring 里提过偏移，
没有定义。`tools/freq/test_freq.py:40-46` 里也出现同一组数字，但那是**断言**（把
`facts.md` §主频读数 的值钉住，否则这条测试是恒真的）。`docs/facts.md` / `docs/map.md` 里的
偏移是给人看的结论，按 AGENTS 的账本纪律保留。

## 3. 两份 `latest_product` 的实际差异与取舍

两份实现的**判名字形状**的方式不同，在现有数据上恰好重合（都得到 `008`），但外推会分叉：

| 目录内容 | 旧 `watch_freq.latest_product`（`name[:3]` + 字典序） | 旧 `bench_rewrite.latest_product`（`^\d+\.opt\.bin$` + 整数 + mtime） | 新 `products.latest_product` |
| --- | --- | --- | --- |
| `1.opt.bin` | `None`（`"1.o"` 不是数字） | `1` | `1` |
| `009.opt.bin` + `0010.opt.bin` | `009`（`"0010"[:3]="001"` 比 `009` 小） | `0010` | `0010` |
| `008.opt.bin` + `1000.opt.bin` | `100`（**截断的编号**） | `1000` | `1000` |

复现：`python work/tmp/freq0/compare_products.py` 的 `[5]` 段（只改模块级 `OPT_DIR`，用合成目录）。

**取舍**：取 bench 那一版的语义（正则匹配 + 按**整数**比编号 + 返回 mtime），因为
① 正确性由编号机制定义（`hook_log._seq3` 三位零填充、跨会话看 mtime），截断成 3 字符在编号
≥1000 或名字不补零时给出**错的编号**；② `wait_new_product` 的「本次编译」判据要用 mtime，
只返回编号的版本没法满足它。返回值因此统一成 `Product(num, path, mtime)`，调用方自己取 `.num`。

现有 8 份捕获（`work/hook/hook_out_001..008`）上三方一致：`008`，mtime `1792228897.048`。

## 4. `hook.log` 三行的解析规则

`products.ownership(num)` 把三类行合成一份 `Ownership`（同号多行取最后一条）：**归属只认日志**，
不读 `opt.ini` 的当前值 —— 后者是「检测时刻」的配置（实测 #005 标 off、#006 标 on，而两者产物
sha 相同）。

| 行 | 键 | 含义 | 归属字段 |
| --- | --- | --- | --- |
| `cfg` | `rewrite=` | 转发层起改写子进程**之前**读到的配置（= 传给改写器的 `--mode`） | `cfg_rewrite` |
| `opt` | `mode=` / `rewritten=` / `sha256=` | 改写器**实际做了什么**（被防御挡回时与 `cfg` 不同） | `opt_mode` / `rewritten` / `sha_log` |
| `impl` | `impl=` / `mode=` / `result=` | `impl` = **实际走了哪个实现**；`mode` = 配的 `compiler`（**不是** rewrite） | `impl` / `compiler_cfg` / `impl_result` |

- 取不到的行一律给占位符：`?`（不知道）/ `-`（没有 sha），不用空串 —— 空串看着像「没有」。
- `impl` 词表（`reference` / `ours` / `fallback` / `ours_failed`）唯一处在
  `docs/specs/2026-10-15-simplexc-interfaces.md` §5，本模块不做白名单（扩容方式 = 只加不改）。
- 旧签名保留：`opt_info(num)` → `(sha_log, rewritten, opt_mode)`、`mode_from_hook_log(num)` →
  `cfg_rewrite`（watch/bench 的语义引用）；新代码用 `ownership()` 一次拿全。
- **路径**：`hook_log_path()` 只从 `hook_log.PATH` 取值（模块里唯一一处引用）。批 2 迁移
  `work/opt/ → work/hook/` 时只改 `hook_log.py: PATH`（+ C 侧 + 文档），这里自动跟着走。

核对（离线）：8 个编号的 `hook.log` `impl` 行与 `manifest.tsv` 的 `impl` 列逐项相同；
`hook.log` 的 `opt.sha256[:8]`、盘上 `NNN.opt.bin` 的 sha256[:8]、`manifest.out_sha256[:8]`
三者逐项相同（`rewrite=off` 时它们同源），见 `compare_products.py` 的 `[2/3]` 段。

## 5. `opt.ini` 的读写规则（fail-safe 与「只改第一条生效行」）

| 情况 | 读（`read_variant`） | 写（`set_variant`） |
| --- | --- | --- |
| 键缺失 | 兜底：`compiler` → `reference`（**绝不能把「没配」当我们的实现**）、`rewrite` → `on`（与转发层/改写器现行兜底一致） | 报错：不新增行（新增行后「第一条」仍是旧值 = 静默失效），提示去 `opt.ini.example` 取消注释 |
| 值非法 | 报错（`ValueError`，带合法值集合） | 由 `Variant.__post_init__` 报错（构造即校验） |
| 多行同名 | 取 `[opt]` 段里**第一条**生效行，与 `GetPrivateProfileString` 和 `optimize_image.rewrite_mode` 一致 | 只改第一条生效行的值，缩进保留、行数不变 |

写法照 `bench_rewrite.set_rewrite_ini`：写 `<name>.new` 再 `os.replace`（转发层每次编译都读这个
文件，读到写了一半的会拿到半截值）；两个键都定位得到才落盘，因此失败时文件字节不变（测试里钉住了）。

## 6. 本批判据

| 判据 | 命令 | 结果 |
| --- | --- | --- |
| 包可导入 + CLI 可用 | `python -c "import tools.freq.probe, tools.freq.variants, tools.freq.products"`；`python -m tools.freq.cli_read --help`；`python tools/read_freq.py --help` | 通过 |
| 偏移唯一 + 数值相同 | `grep -rn "0x14624D6\|0x624D6" --include=*.py tools/`；与 git HEAD 的 `read_freq.py`、树内 `watch_freq.py` 对照 | 见 §2（新包内一处；watch_freq 是冻结副本） |
| variants 往返 + 兜底 | `python tools/freq/test_freq.py`（19 个用例） | 全绿 |
| products 与旧实现一致 | `python work/tmp/freq0/compare_products.py` | 退出码 0（编号/指纹/impl/scan 逐项一致） |
| 旧工具未回归 | `python tools/watch_freq.py --help`；`python tools/bench_rewrite.py -n` | 正常（dry-run 不碰游戏/配置） |

零测试是设计 §2 列的缺陷 6，本批补了 19 个用例（`unittest`：本机没装 pytest，两种入口都支持）。

## 7. 下一批要接的

1. `sampler`：统一口径（median + 丢首个 + 相位对齐 + 停机/低压判据）+ `Segmenter` + 事件 `Sink`；
   `probe.read_speed_time`（相位）与 `probe.snapshot` 已经就位。watch 的 `OPT_WAIT`（见过
   `hook_out` 后等 `opt.bin` 的上限）随它一起搬 —— 本批没搬，避免留下没人用的常量。
2. `report`：`pad`/`fmt_hz`/`bar`/`hz_style` + 区间表 / 对照表 + JSON（`schema=1`）；`products.wait_new_product`
   的进度输出走 `say=` 注入（本批已经留好这个缝），失败解释文本原样保留。
3. `driver` + `cli_bench` / `cli_watch`：`--variant compiler:rewrite`（`Variant.parse` 已就位），
   `set_variant` 的备份/恢复（`atexit` + SIGTERM）与前置断言。
4. `hook.log` 从 `work/opt/` 挪到 `work/hook/`（C 侧 `compile_hook.c` + `hook_log.py: PATH` + 文档）；
   `products`/`probe` 侧**无需改动**（只经 `hook_log.PATH`）。⚠️ 需要重新部署壳。
5. 切旧脚本：删 `watch_freq.py`/`bench_rewrite.py` 的偏移副本、私有 `latest_product`、私有采样循环，
   并把 `bench` 的 mean 口径改成 median（决策 3）。
6. 文档：`docs/map.md` 的「主频监听」行加包指针、`docs/facts.md` §产物 rewrite 模式的权威来源 补 `impl` 行。

## 8. 未验 / 留待批 2

- **与游戏相连的部分未验**：本批全程游戏没在跑，`probe.find_game`/`wait_for_game` 的真路径、
  真内存读取都没跑过；读地址与宽度是用桩 `winmem.rpm` 验的（`test_freq.py` 里断言「读
  `base + OFF_SIM_SPEED` 4 字节」「`snapshot` 读三个字段 4/8/8 字节」）。
- `variants` 的写路径只在临时 ini 上验过，**没碰过真 `opt.ini`**（任务明令只读，md5 前后未变）。
- 本批没装 pytest，故用 `unittest`；`python -m pytest tools/freq/test_freq.py -q` 未实跑。

# hook.log 从 `work/opt/` 迁到 `work/hook/`（freq 设计 §7 决策 4）

- 日期：2026-10-18
- 范围：Python 侧路径解析 + C 侧写入目录 + 加载时清理；**不含部署**（部署由主程做，见下）
- 判据：`python simplexc/tools/check.py` 4/4 PASS（4.82s，`baseline_differs=True`）

## 改了哪几处

| 文件 | 改动 |
|---|---|
| `tools/hook_log.py` | `PATH` 由常量改为**解析结果**（`NEW_PATH` / `LEGACY_PATH` + `resolve()` + 模块 `__getattr__`）；`append()` / `read()` / `find()` 全走 `resolve()`；docstring 换成新路径 |
| `tools/compile_hook.c` | 新宏 `HOOK_LOG_DIR`（默认 = `OUT_DIR` = `work/hook`）；`hook_log()` 的 `CreateDirectoryA` 与 `sprintf(path, "%s%chook.log", …)` 改用它；`purge_captures()` 点名删 `hook.log`；头注释两处路径 |
| `simplexc/tools/hook_offline.py` | `defs()` 多一行 `-DHOOK_LOG_DIR="<case>/opt"` + 头部一句说明（**唯一**的测试侧改动，断言逻辑一字未动） |

为什么 C 侧要单列一个宏：离线判据从 case 的 `opt/` 下读日志（`snapshot()`），新壳默认写到
`OUT_DIR`；不 pin 的话那 8 个 case 的 log 断言会去读一个空文件（判据 2 与判据 3 无法同时成立）。

为什么 B 侧 `purge_captures()` 要点名删 `hook.log`：日志原先在 `OPT_DIR`，被 `purge_workdir`
的 `*.log` 一并清掉；落到 `OUT_DIR` 后那个通配不再覆盖它，而编号（`g_seq`）每次加载从 1 重来
—— 留着上一会话的同号行，`ownership()`「同号取最后一条」会摸到上一会话的值。

## 回退规则（`hook_log.resolve()`，读写同一个入口）

| 盘上状态 | 用哪个 |
|---|---|
| 只有 `work/opt/hook.log` | 旧路径（= 旧壳仍部署时的现场，行为与迁移前逐字相同） |
| 只有 `work/hook/hook.log` | 新路径 |
| 两个都在，**旧**的更晚 | 旧路径（窗口期闭合：旧壳写旧文件 → Python 跟着读旧写旧，不分裂） |
| 两个都在，**新**的更晚（或 mtime 相同） | 新路径 |
| 都不存在 | 新路径（写入时创建；读即空日志） |

- **为什么用 mtime 而不是「恒取新路径」**：过渡期里 C 侧（旧壳）与 Python 侧（改写器
  `hook_log.append`）必须写在**同一个文件**，否则 `cfg` 与 `opt` 分家，`ownership()` 的归属
  静默错位（正是本项目「标注 vs 实际」那条纪律的禁区）。恒取新会在 `append()` 建出新文件的
  瞬间把读写劈成两半。
- **mtime 相同的兜底**：取新路径（确定性，不随扫描顺序抖）。
- 「都不存在」不是错误而是「日志还没出现过」：要判它请用 `os.path.exists(hook_log.resolve())`，
  空的日志与「没编译」不是一回事。`read()` 保持「文件不在 = 空日志」的旧语义（`products` 的
  既有测试就依赖它返回 `[]`）。

## 产物

| | md5 | 大小 |
|---|---|---|
| 迁移前 `build/compile_hook.dll` | `7ff5a8a1d3e15aaaa02d1027bcf70cfc` | 111693 B |
| 迁移后 | `e333401fbf45c2fdc0ebe9fe17f3ac2c` | 112205 B |

`python simplexc/tools/build.py hook` 连跑两次 md5 相同 → 同一路径重编可复现，md5 能当身份判据。
**只编不部署**：我没有碰游戏目录，`F:/opt/...` 与 `opt.ini` 一字未动。

## 主程部署时要做什么

1. 确认游戏**没在跑**（DLL 有文件锁）：`python tools/deploy_compile_hook.py status`。
2. `python tools/deploy_compile_hook.py build-deploy`（它自己重编，命令与 `build.py hook` 同一条，
   产物 md5 应为 `e333401f…`）。
3. 启动游戏，触发一次编译（编辑电路 → `Ctrl+Z`）。
4. 核对（判据）：
   - `work/hook/hook.log` **出现**，且里面有 `cfg` / `opt` / `writeback` 行；
   - `work/opt/hook.log` **消失**（见下面的历史数据）；
   - `python -c "import sys;sys.path.insert(0,'tools');import hook_log;print(hook_log.resolve())"`
     指向 `work\hook\hook.log`；
   - `work/hook/manifest.tsv` 有本次会话的数据行（`work/hook/hook_out_NNN.bin` 同步出现）。
5. 反转/回滚：`python tools/deploy_compile_hook.py restore` 换回旧壳 —— 此时
   `work/opt/hook.log` 会被旧壳重新创建，`resolve()` 因「新文件 mtime 更旧」自动切回旧路径，
   freq 不必改任何东西。

## 旧 `work/opt/hook.log`（2756 B，md5 `5ebd7a0436ae42c5edd09817d2b0f255`）怎么处理

- **默认什么都不做**：新壳加载时 `purge_workdir()` 仍扫 `OPT_DIR` 的 `*.log`，旧日志会被它清掉；
  `purge_captures()` 清 `work/hook/hook.log`。两边都不会留下跨会话的同号行。
- **需要留档就先搬走**：`cp work/opt/hook.log work/freq/hook_pre_move.log`（`work/freq/` 不被任何
  purge 碰）。**不要**把它复制成 `work/hook/hook.log` —— 那正好是「两个都在」的情形，靠 mtime
  才不出错，没必要引入。
- 不删它的理由：「日志的价值也全在历史」，那是唯一一份迁移前的记录（含 `impl` 行的最早样本）。

## 还留着的陈旧路径描述（不在本次允许改的文件里，等主程折账）

- `docs/map.md:24`、`docs/facts.md:587`（账本，主程的）
- `tools/watch_freq.py:60` 注释、`tools/optimize_image.py:71` docstring（另一个任务正在改 freq；后者的
  描述「`work/opt/hook.log` 的 `opt` 行」迁移后不再成立）
- `simplexc/tools/hook_offline.py` 里 `STUB_SOURCE` 的 docstring 有一句「`hook_log.PATH` … 写死在
  `work/opt/hook.log`」—— 迁移后不成立；主程只批准我改 `defs()` 与头部那句，所以这行留着未动。

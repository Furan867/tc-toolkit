# 判据入口 `simplexc/tools/check.py` + 壳离线判据入库

日期：2026-10-17（子 agent 会话）。允许改的文件只有三个：`simplexc/tools/check.py`、
`simplexc/tools/hook_offline.py`、本篇。

## 做了什么

| 文件 | 作用 |
|---|---|
| `simplexc/tools/hook_offline.py` | 壳的离线判据：8 个 case（四种情形 + `refcmp` + 超时两模式 + 大 blob 对照），提供 `run_all()` 与直接 CLI |
| `simplexc/tools/check.py` | 一条命令串起全部离线判据：`corpus` / `host_selftest` / `hook_offline` / `probe_006`（可选） |

`hook_offline.py` 是 `work/tmp/hook_test/harness.py` 的入库版，判据语义逐条照搬。相对前身只改三处：

1. 工作目录统一到 `work/tmp/hook_offline/`（`bin/` `new/` `base/` `results/` 与替身改写器全在下面），
   不再用 `hook_test/`；`work/hook/` 与 `work/opt/` 一次都没被写（路径宏全部 `-D` 过去）。
2. 每个 case 返回结构化结果（`passed` / `checks` / `facts`），`run_all()` 汇总；人读输出仍保留。
3. 替身改写器 `rewrite_stub.py` 由本文件自己落盘 —— 前身用的是 `work/tmp/hook_test/tools/` 里的散件，
   入库版不能依赖不入库的文件。为什么需要替身：真 `tools/optimize_image.py` 的 `hook_log.PATH`
   按自身位置算，写死 `work/opt/hook.log`，离线判据不能往那份活日志里掺行。

## 判据入口的形状

```
python simplexc/tools/check.py                    # 全部步骤
python simplexc/tools/check.py --list
python simplexc/tools/check.py --only corpus --only hook_offline
python simplexc/tools/check.py --json work/tmp/check/last.json
```

- 状态三档：`PASS`（rc=0）/ `FAIL`（rc≠0，决定退出码）/ `SKIP`（前置条件不满足）。
  **SKIP 不算通过也不算失败**；退出码 0 = 没有 FAIL，1 = 有 FAIL，2 = 参数错误（未知步骤名）。
- 一步失败不影响其它步：每步独立跑；`hook_offline` 内部「先编 dll 再跑 case」的先后关系由它自己报
  `skipped`。
- 机器读 JSON：`schema` / `generated` / `git{head,branch,dirty}` / `ok` / `seconds` / `counts`，
  每步 `name` `title` `status` `rc` `seconds` `summary`（键值）`reason` `output_tail`。
- 只用标准库；子进程一律 `PYTHONIOENCODING=utf-8`，否则 GBK 控制台会把摘要变成乱码（已实测：
  不设就能让 `verdict=校验通过` 变成 `У��ͨ��` 那种东西落进 JSON）。

## `probe_006` 是可选步骤

它由另一个 agent 在写，可能正好在被改或在跑长测量。做法是：`--help` 先探一次 CLI（不改它的词表），
按 help 里出现的选项拼一条**不做吞吐测量**的快速调用（`--no-throughput --ticks 3`，输出目录用
`work/tmp/check/probe` 而不是 `simplexc/probe/results`，避免踩它的正式结果文件）。
脚本不存在、`--help` 就挂、依赖缺失、超时、rc≠0 一律 `SKIP` 并写清原因，只试一次不重试。
吞吐测量仍是 manual 的长测量，不该塞进「一条命令」。

本次实测它 `PASS`（`identical=True`，`semantic_diffs=0`），所以 SKIP 分支是另造条件验的：
把模块里的 `PROBE` 换成不存在的路径、换成一个 `--help` 就退 3 的假脚本，两次都得到
`SKIP` + 原因 + `main()` 返回 0。

## 一个必须记住的坑：HEAD 与工作区同源时 `refcmp` 是自比

`refcmp` / `ours-ok` 的对照组是「HEAD 的 `tools/compile_hook.c` + 5 个路径宏 `#ifndef` 化」编出的第二份 dll。
工作区干净时它与工作区同源 → 这两条退化成「自己比自己」，**没有判别力**。结果里的
`baseline_differs` 会如实报 `False`，`hook_offline.py` 的人读输出也会打一行 ⚠。

另一个坑：`core.autocrlf=true` 让 blob 存 LF、工作区是 CRLF，直接逐字节比会把「内容相同」
误报成「改过」（第一次跑就撞上了：`baseline_differs=True` 而 `git status` 干净）。
`baseline_info()` 现在先做 CRLF 归一化再比。

## 反证（判据入口本身也会失败）

| 反证 | 做法 | 实测 |
|---|---|---|
| 参数错误 | `--only nope` | rc=2，列出可用步骤名，不跑任何步 |
| 步骤真失败 | 把 `work/tmp/check/host_selftest` 这个目录占成普通文件（不动任何语料/共享输入） | `host_selftest` FAIL rc=1，其余三步照跑通过，整体 rc=1，JSON 里该步 `rc=1`、`ok=false` |

没有按提示去改名 `corpus/src/006.si`：同一时刻另一个 agent 的 `probe_006` 正在读它，改名会伤到别人的测量。

## 未解决 / 待定

- **`refcmp` 的对照组只对「未提交的改动」有判别力**（见上）。要不要改成对照上一版提交/固定基线，
  需要主程决定 —— 当前照搬前身语义，没有自行加机制。
- `check_offline` 的 `timeout-*` 两个 case 依赖 64 MB 假 blob + 预算 0；机器极忙时 `big-ours-ok`
  （默认 5 s 预算）理论上可能抖动，本次连跑多次都稳。
- 全语料差分（M3）与覆盖率矩阵/回退率还没接进 `check.py` —— 主设计 §7.5 的那条完整的判据链仍缺
  「全语料编译 → 逐 tick 差分」这一环，等前端可用后再加一步（届时 `check.py` 的步骤表就是落点）。
- 游戏内部分（部署转发层、主频采样）保持 manual/slow，`check.py` 里不出现。

## 本次实测总耗时

`python simplexc/tools/check.py` 四步合计约 4.9 s（corpus 0.75 / host_selftest 0.23 /
hook_offline 3.4 / probe_006 0.5）。`hook_offline.py` 单跑 8 个 case 约 3.4 s（含两次 gcc 编壳）。

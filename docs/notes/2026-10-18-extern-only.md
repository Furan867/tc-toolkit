# 覆盖表单列一档 `extern_only`：14 份 → **21 份**（判据是原则性的，多出来的 7 份也是同一回事）

日期 2026-10-18 · 实施者（唯一写者：`simplexc/tools/m5/coverage.py`）· 现场 `work/tmp/externonly/`
口径（重跑后的现行值）：**75 可装配 · 21 全等 · 21 只差 extern · 33 diff · 23 cli_fail**（21+21+33+23=98）。

## 0. 结论先行

- 负责人批准的那一档（「内存逐 tick 等价、只差 extern 就不算值不对」）**已实现并单列**；
- 但按原则性判据跑下来命中的是 **21 份，不是 14 份** —— 差集 7 份 = **042 044 094 124 133 135 137**。
  负责人当场定为**认 21**（理由：判据是原则性的，那 7 份满足它；「按 id 写白名单」不进任何判据）；
- 多出来的 7 份**不是判据写松了**：它们在 18:30 那份日志里是真差异，**18:49 的 chkstk 修复（`002ca34`）之后
  我们追平了参考**（§3 的哈希对照）。所以那次修复的收益比当时按 `host_fail=9→0` 记的更大。

## 1. 判据（全部满足才算 `extern_only`）

1. tick 条数相同；
2. **每个 tick 的段哈希 / 页 / `sim_cycle` 全等**；
3. extern 序列**至少有一处不同**。
   满足 2 + 3 而只是 extern 不同 ⇒ 单列一档；任何别的差异（内存、段哈希、`sim_cycle`、tick 条数）照旧算 `diff`。

实现上有一条**刻意**的选择：**不复用 `selftest.compare` 的输出来判**。它到 8 条差异就收口（`if len(diffs) >= 8: break`），
只看它会把第 9 条往后的内存差异漏成「只差 extern」；`extern_only_split()` 逐 tick 全看一遍（`coverage.py`，
返回 `(是否只差 extern, 审计内容)`）。

extern 差异**没有被藏起来**：

- TSV 新列 `extern_detail`（`extern ref 2 条（VirtualAlloc×2）/ ours 0 条（无）· 差异 tick [1]` 这种形态）；
- 汇总 JSON 的 `extern_only` / `extern_only_ids` / `extern_only_details`；
- 报告里**单列一节**「只差 extern（`extern_only`）」：写清含义 + 逐份的 extern 差异（条数、符号名、哪些 tick）。

```text
$ python simplexc/tools/m5/coverage.py --out-dir work/tmp/externonly/full --tsv work/tmp/externonly/full/coverage.tsv \
    --summary-json work/tmp/externonly/full/coverage.summary.json --report work/tmp/externonly/full/coverage.md
汇总：总 98 · 可装配 75 · 逐 tick 全等 21 · 只差 extern 21 · cli_fail 23 · diff 33 ·
      装配回退率 23.5% · 差分回退率 57.1%（不含只差 extern）
```

（`--report` 必须显式给：默认值指向仓库里那份 98 份报告，子集/另跑会把它盖掉 —— `pitfalls.md` 有这条。）

## 2. 汇总口径的变化（单列，不混进 diff）

| 位置 | 改前 | 改后 |
| --- | --- | --- |
| 汇总 | 逐 tick 全等 21 · 差分回退率 **78.6%**（77 份不能全等） | 逐 tick 全等 21 · **只差 extern 21** · 差分回退率 **57.1%**（56 份：不能全等且不是只差 extern）· 另记「不能逐 tick 全等（含只差 extern）77（78.6%）」 |
| 失败分类表 | `extern_only` 份按 `TickDiff/extern` 混在里面 | 不进失败分类（那 21 份另有专门一节）；剩下的 `TickDiff/extern 1 份 = 096`（它另有内存差异，照旧算 diff） |
| 状态列 | `diff` | `extern_only`（新状态，属「装配成功」集合） |

`差分回退率` 去掉 `extern_only` 是**口径变化**，不是放松判据：判据（§1）一字未松；旧口径的值仍以
`rate_not_tick_equal` 记在汇总 JSON 里、也仍印在报告里，可对账。

## 3. 「14 → 21」的时间线（负责人点名要写进来的那条）

18:30 的 `coverage.base.tsv`（旧日志）里，那 7 份还不是「只差 extern」：6 份（042 044 094 133 135 137）有
`0x232aa920000`（`ui_buffer`）段差 + 页差，124 是 `host_fail`（我们的产物崩，重试 4 次仍失败）。
对比现在的日志（同 CLI：`build/simplexc_cli.exe` md5 `79064948e7d0679fa6b93dba04c045a9`）：

| id | 18:30 状态 | 18:30 参考哈希 | 18:30 我们哈希 | 现在两边共同哈希 | 是参考退化还是我们追平 |
| --- | --- | --- | --- | --- | --- |
| 042 | diff | `55b26cc4b163b7f2…` | `08acf8d206c81966…` | `55b26cc4b163b7f2…` | **我们追平参考** |
| 044 | diff | `ed2b45d3fcf6a682…` | `85d23e596e3b94d0…` | `ed2b45d3fcf6a682…` | 同上 |
| 094 | diff | `1607b2c7ac9bd39f…` | `b949adc43f3dd853…` | `1607b2c7ac9bd39f…` | 同上 |
| 133 | diff | `332e8b5633a5313d…` | `85db61711491afe3…` | `332e8b5633a5313d…` | 同上 |
| 135 | diff | `c4830deb60e05c21…` | `85db61711491afe3…` | `c4830deb60e05c21…` | 同上 |
| 137 | diff | `c9b4a3cd804c0bad…` | `c63de336c0cc0891…` | `c9b4a3cd804c0bad…` | 同上 |
| 124 | host_fail | （没有哈希） | （崩） | `85f5a31248448ff8…`（两边相同） | 我们不再崩 |

参考侧的哈希**一个都没变**，变的是我们侧 —— 追平发生在 18:30 → 19:04 之间那次改动，即 18:49 的
`002ca34`（`___chkstk_ms` 改用 `%rcx` 走查，不再破坏 `%r10`）。当时这笔只记了「9 份 host_fail 归零」，
从这份数据看它同时把 6 份的 `ui_buffer` 渲染差异也消掉了。**未证**：没有逐份二分到「就是 `%r10` 那条」，
依据是时间窗 + 该提交改的是产物里的栈走查桩。

## 4. 逐 id 差集（判据 1 的原始输出）

`work/tmp/externonly/diff_ids.py`：基线 `work/tmp/m5/coverage.tsv`（19:04，旧口径）vs 新跑全量。

```text
总 99 行（98 语料 + rvsim 基准行）
分类变了 21：042 044 063 065 073 075 076 077 078 094 124 126 128 129 133 135 137 145 151 153 155
             （全是 diff → extern_only，没有一份反向）
仍是 diff 33：001 008 010 012 014 016 018 022 024 047 048 049 050 051 053 054 056 060 061
             096 098 100 102 104 106 108 110 112 114 116 157 178 179
非 diff 的行：status 与 diff_lines 一字未动
status 相同但 diff_lines 不同的：无
```

14 份原名单（063 065 073 075 076 077 078 126 128 129 145 151 153 155）**全在**上面这 21 里。

## 5. 其它判据

```text
$ python simplexc/tools/check.py --json work/tmp/externonly/check.json
corpus PASS · host_selftest PASS · lower_equiv PASS（10 tick 全等）· hook_offline PASS（8/8）· probe_006 PASS
总计 5 步：5 PASS · 0 FAIL · 0 SKIP    用时 36.63s
```

pi-lens：本次改动**没有新增命中**（没引入新的文件 API 调用点）。`coverage.py` 里原有 12 条
（10 × `python-path-traversal` + 2 × `python-empty-except`，全是 tree-sitter 档的 warning、全是既有行）
与 `2026-10-18-ir-build-lint.md` §「还剩什么」-1 同一个待定口径，没动。

## 6. 遗留

1. 判据只到「段哈希/页/`sim_cycle` 全等」这一层 —— **不等于**游戏内行为等价（extern 条数本身可能影响节奏）。
   要更强的说法得另立判据（例如把 `VirtualAlloc` 这类分配器 extern 归一到「不影响语义」的白名单）。
2. `ui_buffer` 那 6 份追平里的因果**没定死**（见 §3 末）。
3. `extern_only` 不进失败分类表，所以失败分类的「份数合计」不再等于「非全等份数」—— 读表时按 `by_status` 对账。

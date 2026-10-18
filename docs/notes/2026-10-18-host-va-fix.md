# 宿主 VirtualAlloc 桩：只 RESERVE 的请求不再当 COMMIT 真提交（20 份 HostRc → 可判定）

生成 2026-10-18 · 改动只有 `simplexc/tools/host/host.py` · 复验脚本在 `work/tmp/hostva/`
起因：`docs/notes/2026-10-18-tickdiff-diagnosis.md` §2（假设 B，含机制与证据）

## 0. 结论

| 项 | 改前 | 改后 |
| --- | --- | --- |
| 20 份 HostRc 的**参考产物**跑 10 tick | 0 tick（AV，`产物 +0x6BC`×15 / `+0x1FBF`×5） | **10/10 tick，rc=0，ref/ref 逐 tick 全等** |
| 覆盖表里的 HostRc | 20 | **0**（那 20 份落到 TickDiff） |
| 其余 71 份（有旧日志的） | — | 一字不变 28 · 由崩变跑通 20 · 只 extern 变 23 · 回归 **0** |

判据器材只**修正错误**、没放松任何一条：比较规则（段哈希 / 页 / sim_cycle / extern）一个字没动，
`_virtual_alloc` 只是不再把 `MEM_RESERVE` 当成 `MEM_COMMIT`。

## 1. 复现（改前两次、改后一次）

```text
python simplexc/tools/host/host.py --src simplexc/corpus/src/034.si --bin simplexc/corpus/ref/034.bin --ticks 10
  [host] 异常 code=0xC0000005 addr=0x20E423A06BC（产物 +0x6BC） params=[1, 0]          ← 崩
python work/tmp/tickdiff/probe_va.py 034 ref 3                                        ← 透传真实 API
  == tick 1/2/3 reason=ok；6 次 VirtualAlloc(0x0, 0x2540BE400, 0x2000, 0x1) 全成功
python work/tmp/hostva/old_semantics.py 001 3        # 把改动前的实现装回内存里的类
  [host] 异常 code=0xC0000005（产物 +0x6BC）params=[1, 0]                              ← 崩
```

同一份产物、同一个驱动，只有那一个桩的实现不同 → 崩不崩由**桩的语义**决定（与产品代码无关）。

## 2. 修了什么

`_virtual_alloc` 现在按 `typ` 分三种：

| `typ` | `addr` | 行为 |
| --- | --- | --- |
| 含 `MEM_COMMIT` | 0 | 宿主 bump arena（原样，确定性） |
| 只含 `MEM_RESERVE` | 0 | **`_reserve`**：从固定首址顺序切地址、一页都不提交 |
| 其余 | ≠0 | 透传真实 API（地址由产物给，本来就确定） |

`_reserve` 的保留区首址固定 `0x400000000000`（64 TiB），按需一块块保留（每块 256 GB）：
返回值要进 `extern` 记录、两遍跑逐字节比，**不能让系统挑地址**。保留不出来时打硬错误并返回 0
（桩跑在产物线程上，抛异常会被 ctypes 吞掉）——「吵」而不是静默。

参考产物因此走它本来要走的路：RESERVE 拿到非空基址 → 之后在**里面** `MEM_COMMIT(0x4000)`
（实测 042：6 次 RESERVE 全成功 + 2 次 COMMIT，洞见 §5 的时序注记）。

## 3. 复验

```text
python work/tmp/hostva/refcheck.py 10      # 20 份：ref×2 + ours×1（10 tick，--seed-reset-data seq）
  == 20 份里 ref/ref 不等：0        （跑了两遍，逐 tick 全等的分别是同一批 20 份）
  20 份 ref/ours 全部给出结论（TickDiff，首差异段 19 份 = 0x232a6ff0000 / 100 = 0x232a9f50000
  —— 与 §A 的 in_scope 同形，根因在 lowering 那条线上，不归宿主）
python work/tmp/hostva/regress_equal.py    # 71 份有改动前日志的：逐 tick 比改动前的 ref 日志
  一字不变 28 · 由崩变跑通 20 · 只 extern（页/段/sim_cycle 全等）变 23 · 回归 0
python simplexc/tools/check.py             # 5 PASS · 0 FAIL · 0 SKIP（host_selftest 步 PASS）
```

## 4. 预期变化

- 20 份 HostRc 从「判不了」变成「可判定」，且**都报 TickDiff** → 98 份的口径变成
  `10 全等 + 60 TickDiff + 28 cli_fail`（原来 `10 + 40 + 20 + 28`）。它们与那 40 份同因（`in_scope`）。
- 034 / 090 在 `status.md` 里记的「间歇崩溃」按本次口径不再存在（是同一个桩语义问题）。

## 5. 遗留风险

- **extern 字段有时序成分**（不是我这次引入的，但被我改变了相位）：`Host.tick()` 用
  `before = len(records)` 切片，产物线程在宿主调到 `tick(1)` 之前的 extern 调用**不进任何快照**。
  修好 RESERVE 之后那 6 个保留区调用瞬间完成，于是 23 份从「留在 tick 1 快照里」变成「被切掉」——
  页/段/sim_cycle 仍全等。证据（042，同一份产物、同一 10 tick，只有驱动快慢不同）：

  | 驱动 | tick(1) 前 records | 快照里的 extern |
  | --- | --- | --- |
  | 进程内探针（`prefix_probe.py`） | 0 | 8（6 RESERVE + 2 COMMIT） |
  | 像 CLI 那样在 tick(1) 前先 `open(--out)` | 6 | 2（只剩 COMMIT） |

  同一驱动下稳定（CLI 6+ 遍都是 6/2；ref/ref 全等）。**若某份的差异只有 extern，别当成内存语义差异**；
  要不要把这段前缀算进 tick 1（改「切片的定义」）是主程的口径决定，本次没动。
- 保留区依赖 64 TiB 起的大块 `MEM_RESERVE` 在本机可用（实测 4 块 ×256 GB 连保留 + 里面 COMMIT 可写）；
  不可用时打硬错误并返回 0，不静默换地址。
- `VirtualFree` 仍是桩（返回 1、不真释放）：释放后再留不回收，地址只增不减。
- 只验了「有旧日志的那 71 份 + 那 20 份」；28 份 cli_fail 与宿主无关（`'{'` 12 等）。
- 20 份的**逐字节**差异归属（`in_scope`）没在本次收口 —— 那是 lowering 那条线的判据。

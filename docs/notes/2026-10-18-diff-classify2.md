# 24 份 diff 的重新分类（第二版）

日期 2026-10-18 · 诊断者（**只读** `simplexc/**`，未改一个字节；有写者正在改 `lower/`）· 器材 `work/tmp/diffclass2/**`（不入 git）
口径（现行，`work/tmp/recheck5/s.json`，19:51 全量 98 份）：**75 可装配 · 30 全等 · 21 只差 extern · 24 diff · 23 cli_fail**。
本文只处理这 **24 份**：`001 047 048 049 050 051 053 054 060 061 096 098 100 102 104 106 108 110 112 114 116 157 178 179`。

**先做粗分类，不重跑语料**：只读 19:51 那份日志（`work/tmp/recheck5/logs/*.jsonl`）逐 tick 全 10 tick 复算
（`scan2.py`，不复用 `selftest.compare` 的 8 条收口）；只有 7 份（001 049 060 100 104 108 178）另跑过宿主做字节级取证
（`probe2.py` = `tickdiff/probe.py` 的手改副本，只把「我们的产物」指向 `recheck5/out/`）。

## 0. 分类表（结论先行）

| 类 | 判据（复算出来的差异字段集合） | 份数 | 语料 | 首差异段 |
| --- | --- | --- | --- | --- |
| **A′ 测试结果链残部** | `sim_test_result` ours=**2(fail)** vs ref=0，`sim_cycle` ours **恒落后 1**；三/四段同时差 | **7** | 060 061 096 098 100 102 157 | `0x232a6ff0000` |
| **B′ `ui_buffer` 文本渲染** | 唯一差异段 `0x232aa920000`；电路三段 + `sim_cycle`/`sim_test_result` **全等** | **8** | 001 047 048 049 050 051 053 054 | `0x232aa920000` |
| **C′ SIM_STATE 线值 ±1** | 唯一差异段 `0x232a9f50000`；无 `sim_cycle`/`sim_test_result` 差 | **5** | 108 110 112 114 116 | `0x232a9f50000` |
| **D′ 同 C′ + 别处** | `0x232a9f50000` 差 + `0x233089a0000`（178 179）或 `ui_buffer` 单字节（104 106） | **4** | 104 106 178 179 | `0x232a9f50000` |

7 + 8 + 5 + 4 = 24 ✓

- **按份数最大的是 B′（8 份）**，但它与「值算错」无关：电路三段逐字节全等、`sim_cycle` 全等，差的是
  `ui_set_text` 渲染出的文本。**按已知机理最大的是 A′（7 份）**——而**在修的那 3 份只覆盖它的一半**（见 §2）。
- **任务里点名的 060/061/098 落在 A′**（已标出）；同类的另有 **096 100 102 157**，那 3 份的修法**大概率覆盖不到**它们。
- 上一轮的 **E 类（我们产物崩，9 份）** 已随 `%r10` 修复归零：其中 8 份（001 047 048 049 050 051 053 054）**落到本轮的 B′**，
  第 9 份（124）落到 `extern_only`。**E→B′ 不是同一件事**（崩没了、但 UI 文本仍差）。
- 上一轮的 **D 类（6 份 `ui_buffer`）** 已随同一次修复归零（`externonly.md` §3），本轮 B′ 是**另一批** id。

## 1. A′（7 份）：`check_output` 的 fail 分支在我们这边先触发

四段同时差、`sim_test_result` ours=**2**（fail）、`sim_cycle` ours 比 ref **恒少 1** —— 与上一轮 A 类**同一条链**
（`handle_test_result` 里 `halt()` ⇒ 本轮不再 `cycle += 1`）。tick 1 的差异字节（`diffbin.py`，代表 060）：

```text
060 t1 段 0x232A6FF0000 差异 3 / 1114112
  0x232A7000000 (+0x10000) ref=01 ours=00   [settings+0x0  = sim_cycle]
  0x232A7000010 (+0x10010) ref=00 ours=02   [settings+0x10 = sim_test_result]
  0x232A7010000 (+0x20000) ref=0a ours=00   [input_replay[0]]
060 t1 段 0x232A9F50000 差异 52 / 1048576 ；段 0x232AA920000 差异 39
```

**决定性的一份是 100**（`check_output` 体：`if get_memory("Count") != cycle + 1 { … return fail }`）：
我们的 `error_buffer`（`0x232AA920000`，长度字段在 `+0`）里出现了**我们写进去的**失败文本：

```text
100 t1 0x232AA920000 (+0x0) n=1  ref=00 ours=1a        ← 长度 26
100 t1 0x232AA920002 (+0x2) n=26 ref=0000…00 ours=e8aea1e695b0e599a8e79a84e580bce5ba94e4b8ba2031e38082
        （UTF-8 = 「计数器的值应为 1。」）             ref 侧 error_buffer 整段为 0
```

⇒ 100 的 fail 分支在 `cycle=0` 那一轮就进了（`expected_value = cycle + 1 = 1`，而 `get_memory("Count")` 读到的不是 1）。
**我们的产物侧内容与已修好的 9 份（008/010/…/056）修前逐字相同**（`0x232a6ff0000` ours = `14ddcdb39b211a82…`）：

| id | ref 哈希（`0x232a6ff0000`） | ours 哈希 |
| --- | --- | --- |
| 060 061 100 102 157 | 各不相同（`b7fb9169…` / `b27af0b0…` …） | **同一份 `14ddcdb39b211a82…`** = 已修 9 份的**修前**值 |
| 098 | `546605a391f95fdc…` | `8d71644220de0d7e…`（与上面那批不同） |
| 096 | tick 1 **只有 extern**；tick 2 起 `sim_test_result` 0/2，tick 3 起 `sim_cycle` 落后 | — |

⇒ A′ 的机理与 `2026-10-18-test-result-chain.md` 那一条**同族**，但**不是同一处**：那份修的是「数组字面量缺 8 字节 Nim 头
⇒ `.len()` 是 undef ⇒ 整条 `if` 被折掉 ⇒ `check_output` 恒 `ret 2`」。**A′ 的 7 份没有 `.len()`**：
060/061 是 `cycle == 0xff`、100 是 `get_memory("Count") != cycle + 1`、096/098/102 是
`output.output_enabled && (opcode != #MOVE >> 6 || to != 6)`、157 是 `output_index >= outputs.len()`。
⇒ 共有现象是「fail 分支先触发」，**共同机理未定**（我方侧产出的字节与「常量 fail」那条同形，值得先看这 7 份的 `--ir-out`）。

⚠ **096 的坑**：它 tick 1 **只有 extern 差**（无内存差），差异从 tick 2 才开始 ⇒ 只看 tick 1 会把它误判成
「只差 extern」（`test-result-chain.md` §0.4 就是这么写的）。要判它必须看满 10 tick。

## 2. B′（8 份）：`ui_buffer` 里少了一整格渲染（电路全等）

001 与 049 逐字节（段 `0x232AA920000`，文本窗口起点 `+0x2D4D0`，长度字段在 `+0x2D4C0`）：

```text
001 t1 段 0x232AA920000 差异 108 / 1114112
  0x232AA94D4C0 (+0x2D4C0) ref=47 ours=32          ← 文本长度：ref 71 字节，我们 50 字节
  ref : [right]    [T][T][F][F][F][F][T][T]    [T][T][F][F][F][F][T][T][/right]   (71)
  ours: [right]    [T]    [T][T][F][F][F][F][T][T][/right]                        (50)
049 t1 同一地址同一形态：ref = [right]    [F]×8    [F]×8    [/right]（71）
                        ours = [right]    [F]    [F]×8    [/right]（50）
  段 0x232A6FF0000 / 0x232A72A0000 / 0x232A9F50000 三段的差异字节 = 0；sim_cycle / sim_test_result 全等
```

源码（`001.si:11620` / `11635`，049 同形）：``ui_set_text("table[4][2]", `[right]    {get_binary_repr(Int output.carry_out, 1)}    {get_binary_repr(Int output.output, 8)}[/right]`)``。
⇒ **两个样本里参考的两段渲染都逐字相同**（001 两段都是 `TTFFFFTT`，049 两段都是 8×`F`），
而按 `get_binary_repr` 的定义（`001.si:11580`，`i = size-1` 递减）`size=1` 只该出 1 格。
最贴的推测是**同一字符串字面量里两次插值调用串味**（参考侧第二次调用的实参/结果覆盖了第一次的渲染）；
备选是「`ui_buffer` 的 4 个滚动槽内容其实来自不同 tick，我们读到的窗口偏了一帧」。本报告没做区分实验，见 §4。

## 3. C′（5 份）与 D′（4 份）：`#SIMULATION_STATE` 线值落后

```text
108 t1 段 0x232A9F50000 差异 57 / 1048576（其余段全 0）
  0x232A9F5016B (+0x16B) ref=0E ours=0F  ；0x232A9F50184 ref=06 ours=07 ；0x232A9F5018A ref=00 ours=01
  0x232A9F501A4 ref=01 ours=00          ⇒ 与上一轮 C1（110）同一批，±1
104 t1 段 0x232A9F50000 差异 19 + 段 0x232AA920000 差异 1
  0x232A9F501BD (+0x1BD) ref=FE ours=FF ；0x232A9F501F0 ref=00 ours=01 ；0x232AA930040 ref=FE ours=FF
178 t1 段 0x232A9F50000 差异 38
  0x232A9F5053F ref=01 ours=00 ；0x232A9F50547 ref=04 ours=00 ；0x232A9F50557 ref=08 ours=04
  ⇒ 不止 1 档（0→4、0→8），像是落后若干 tick
```

三份的 `sim_cycle` / `sim_test_result` / `input_replay` 全等 ⇒ **不是 A′ 的下游**，是这条电路的线值本身落后；
108/110/112/114/116 的 ours 哈希逐字相同（`4a58efc101280efb…`，参考 `89085701d4cc4b41…`）⇒ 这一族是同一个内容。

## 4. 遗留风险

1. **A′（7 份）的机理未定**：观测级结论是「fail 分支先触发」，且我方产出的 `0x232a6ff0000` 字节与
   「常量 fail」那条修前的值**逐字相同** ⇒ 先怀疑同一处 IR 折叠，但 7 份的 `check_output` 体**没有 `.len()`**。
   下一步应先 `--ir-out` 看这 7 份（尤其 100：`get_memory("Count")` 的取值）。
2. **正在修的 3 份 ≠ A′ 全部**：060/061/098 之外还有 096 100 102 157。096 只有看满 10 tick 才现形。
3. **B′ 的机理未定**（§2 两个假设）。它落在「`ui_set_text` 的插值渲染」上，与 `status.md` 下一步 3
   （前缀渲染族 `hex/str/bin`，`stdlib_prefix.si:1110-1234`）**可能是同一批语义**，但没有对照实验。
4. 本轮只逐字节验了 7 份（001 049 060 100 104 108 178），其余 17 份是按「首差异段 + 差异字段集合 + 同形哈希」外推的。
5. B′ 的 8 份在 tick 1 同时有 `extern 2 vs 0`（参考侧 `VirtualAlloc`）—— 与段差无关，别混算。

## 5. 复现

```bash
python work/tmp/diffclass2/scan2.py                          # 粗分类（只读 recheck5 日志）：scan2.log / scan2.json
for id in 001 049 060 100 104 108 178; do for s in ref ours; do \
  python work/tmp/diffclass2/probe2.py $id $s 1 --dump-regions work/tmp/diffclass2/$id/reg.$s; done; done
python work/tmp/diffclass2/diffbin.py 001 1 work/tmp/diffclass2/001/reg.ref work/tmp/diffclass2/001/reg.ours 6
python work/tmp/tickdiff/diffreg.py 108 1 work/tmp/diffclass2/108/reg.ref work/tmp/diffclass2/108/reg.ours
```

（`probe2.py` 与 `tickdiff/probe.py` 的差别只有一处：我们的产物取 `recheck5/out/<id>.bin` + `.bin.entry`，
不再读 19:04 的 `m5/coverage.tsv` —— 那份的 `entry` 与现行产物不匹配。）

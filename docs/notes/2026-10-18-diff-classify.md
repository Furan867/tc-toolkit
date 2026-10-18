# 49 份「装得出、逐 tick 不等」的分类（含 5 份深挖）

日期 2026-10-18 · 诊断者（**只读** `simplexc/**`，未改一个字节）· 器材 `work/tmp/diffclass/`（不入 git，本文自带原始输出）
口径：`70 可装配 · 21 全等 · 49 diff · 28 cli_fail`；本报告只处理这 49 份。
**先做粗分类，不重跑语料**：只读已存的 `work/tmp/m5/{coverage.tsv,logs/*.jsonl}` 复算（`scan.py`），
只有 4 份（010/024/110/042）另跑过宿主做字节级取证。

## 0. 分类表（结论先行）

| 类 | 判据（复算出来的差异字段集合） | 份数 | 语料 | 是语义差？ |
| --- | --- | --- | --- | --- |
| **A 测试结果链** | 首段 `0x232a6ff0000`（`settings+0x0`/`+0x10`、`input_replay[0]`）+ `sim_test_result` 差 | **12**（+096 = 13） | 008 010 012 014 016 018 022 024 056 060 061 098（096 从 tick 2 起同形） | **是** |
| **B 只差 extern** | 全程唯一差异 = `extern 条数 1 vs 0`；段/页/`sim_cycle` **全部逐 tick 全等** | **14** | 063 065 073 075 076 077 078 126 128 129 145 151 153 155 | **否**（参考自己的分配器调用） |
| **C1 SIM_STATE 值差** | 唯一差异段 `0x232a9f50000`，`sim_cycle`/`sim_test_result` 全等 | 5 | 108 110 112 114 116（5 份 ours 哈希**相同**） | 是 |
| **C2 同上 + 多一段** | `0x232a9f50000` + `0x233089a0000` | 2 | 178 179 | 是 |
| **D UI 文本** | 差异只在 `0x232aa920000`（`ui_buffer` 的文本），电路三段全等 | 6 | 042 044 094 133 135 137 | 是（渲染） |
| **E ours 自己崩** | `host_fail`：**我们**的产物 AV | 9 | 001 047 048 049 050 051 053 054 124 | 是（第二层） |

13 + 14 + 5 + 2 + 6 + 9 = 49 ✓

- **按份数最大的是 B（14 份），但它不是语义差异** —— 段哈希/页/`sim_cycle` 逐 tick 全等，只差参考产物自己
  调了一次 `VirtualAlloc`（我们产物不调）。**真实缺口里最大的是 A（13 份）**；A + D 同源（都在
  `check_output` → `handle_test_result` → 状态/文本这条链上），合起来 **19 份**。
- 早先 `tickdiff-diagnosis.md` 的 `in_scope` 根因**确已修掉**：现在 `if in_scope("check_output")` 的守卫
  在我们这边判**真**（实验证据见 §4），005（`check_output` 体为空）已全等。
- 12 份新迁入（闭包修完后）落点：**A** = 024 098 096 · **B** = 075 076 077 078 · **C1** = 108 110 112 114 116。

## 1. A 类（13 份）：`check_output` 的失败分支比参考先触发

深挖 2 份代表（**024 是新迁入的**；010 与 024 逐字节同形）。器材：
`python work/tmp/tickdiff/probe.py <id> <ref|ours> 1 --dump-regions work/tmp/diffclass/<id>/reg.<side>`
→ `python work/tmp/tickdiff/diffreg.py <id> 1 <refdir> <oursdir>`（tick 1，两边都灌 `--seed-reset-data seq`）。

```text
010 t1 0x232A7000000 (+0x10000) ref=0x01 ours=0x00  [settings+0x0 = sim_cycle]
010 t1 0x232A7000010 (+0x10010) ref=0x00 ours=0x02  [settings+0x10 = sim_test_result]
010 t1 0x232A7010000 (+0x20000) ref=0x01 ours=0x00  [input_replay[0]]
010 t1 0x232A9F50100 (+0x100)   ref=0x01 ours=0x00  [SIMULATION_STATE+0x100]
010 t1 0x232A9F50101 (+0x101)   ref=0x01 ours=0x00
  [010 t1 段 0x232A6FF0000] 差异字节 3 / 1114112     [0x232A9F50000] 2 / 1048576
024 t1 同上 3 字节 + output_history_pins+0xA ref=0x01 ours=0x00 + SIM_STATE 0x100/101/10A..10D ref=01 ours=00
```

哈希（`coverage.tsv`，A=参考 / B=我们）：010/012/014/018/022/024/056 的 A = `2517ab16f87a203c…`、
008/016 的 A = `24ef672779f9b1b0…`、060/061 的 A = `b7fb91690ca8c036…`，这 **11 份的 B 逐字相同**
（`14ddcdb39b211a82…`）⇒ 我们侧在这一段落成了同一个内容；098 的 B 不同（`8d71644220de0d7e…`）。快照侧：`sim_test_result` ours=**2（fail）** vs ref=0（pass）/1（win），
`sim_cycle` ours 比 ref 恒**少 1**（022/024 是 10/10 tick，010 是 3/10 tick）。

源码语义点（`corpus/src/010.si`，024 同形）：

```text
320-331  def handle_test_result(result: TestResult) None {
             if result != pass { var res = result; halt()
                 set_setting(sim_test_result, max(get_setting(sim_test_result), U64 res)) } }
338-345  while cycle < burst_target_cycle {
340          if in_scope("get_input") { level_input = get_input(cycle + 1) }
347-350      var vid256 = U1 level_input.input & 1 ; … ; input_replay[0] = U64 level_input.input
382-385      if in_scope("check_output") {
                 let result = check_output(cycle + 1, level_input, level_output)   // ← 语义点
                 handle_test_result(result) }
400          cycle += 1
461-475  def check_output(cycle: Int, input: Input, output: Output) TestResult {
             if output.output_is_z { return fail }
             if #CORRECT_OUTPUT[cycle] != output.output { return fail }
             if cycle == #CORRECT_OUTPUT.len() - 1 { return win }  }   // 无显式 return：fall-through
```

复算：ref 在 tick 1 跑 **2 轮**（cycle −1→0→1），第 2 轮 `check_output(1)` 走到 `return win`（008）
或 fall-through 的 pass（010）⇒ `test_result` 不变成 fail，`cycle` 正常 +1；ours 在第 **1 轮**
（cycle=−1，`get_input(0)` ⇒ `input_replay[0]=0`）就进了 **fail 分支** ⇒ `handle_test_result` 里
`halt()` ⇒ 本轮不再 `cycle += 1` ⇒ `sim_cycle` 恒落后 1、`input_replay[0]` 停在 0、下游
`SIMULATION_STATE+0x100/101` 跟着 0。**一个分支的取反解释了这类全部的观测字节。**

反面证据（同一份语料家族）：**005 全等**，它的 `check_output` 体是 `{ }`（永远 fall-through），
008/010/024 的体里有**可达的 fall-through 路径**且读 `output.*`/`#CORRECT_OUTPUT[cycle]`。
⇒ 差异不在「守卫有没有展开」（守卫我们判真），而在 **`check_output` 体里那个条件的求值/返回值**。

## 2. C 类（7 份）：`#SIMULATION_STATE` 里 ±1 的线值差（与测试结果无关）

代表 **110**（新迁入；108/110/112/114/116 的 ours 哈希逐字相同 = `4a58efc101280efb…`，参考 `89085701d4cc4b41…`）：

```text
110 t1 0x232A9F50168  ref=0x01 ours=0x00     (0x168,0x169)
110 t1 0x232A9F5016B  ref=0x0E ours=0x0F     (0x16B..0x16E)
110 t1 0x232A9F50184  ref=0x06 ours=0x07     (0x184..0x189)
110 t1 0x232A9F5018A  ref=0x00 ours=0x01     (0x18A,0x18B)
110 t1 0x232A9F501A4  ref=0x01 ours=0x00     (0x1A4,0x1A5)
  [110 t1 段 0x232A9F50000] 差异字节 57 / 1048576（其余四段 0）
```

`sim_cycle`/`sim_test_result`/`input_replay` 全等，只有状态位差 ±1（`0x0E→0x0F`、`0x06→0x07`、`0x00→0x01`）
⇒ **不是 A 的下游，是这条电路的线值算错了一档**。110 的关口链是
`root → mode_run → handle_test_result → get_input_switched → seen`（见 `closure-abi.md` §2），
怀疑点落在 **`#if in_scope("get_input_switched")`（110.si:3222）这块的取值/闭包捕获**，
但本报告只到「字节级 ±1」这一层，没定位到具体那一条语句。

## 3. D 类（6 份）：`ui_buffer` 的状态文本没渲染出来

代表 **042**（`0x232AA920000` 段；`ui_buffer` 文本窗口 `+0x17538`，tick 1）：

```text
ref : 5b 54 5d 5b 46 5d 5b 46 5d 5b 46 5d 5b 46 5d 5b 46 5d 5b 46 5d 5b 54 5d   "[T][F][F][F][F][F][F][T]"
ours: 00 00 15 00 00 00 00 00 00 00 12 00 00 00 00 00 00 00 0f 00 00
哈希 A=55b26cc4b163b7f2…（参考）/ B=08acf8d2c81966b7…（我们）
```

- 参考在 **4 个滚动偏移**（`0x232AA937538`、`…3EA68`、`…45F98`、`…4D4C8`）各写 21 字节 `[T]/[F]` 文本；
  我们的那 4 个窗口只有 3 个杂散字节（`0x15/0x12/0x0F`），**整段搜不到 `5b 54 5d 5b 46 5d`** ⇒
  我们这条 24 字节长的 `Str`（长度字段 `+0x17530` 两边都是 `0x18`）**内容没落成 `[T]/[F]`**。
- 同一 tick 的前三段（`6ff0000`/`72a0000`/`9f50000`）**逐字节全等** ⇒ 电路仿真一致，差的是
  测试结果文本（`set_text` → `ui_buffer`）。042 的 tick 1 还有 `extern`（见 B 类）。
- 怀疑与 A 同源（都经 `handle_test_result`），但本报告没证；下一步归到「`Str` 写入 `ui_buffer` 的那条路径」。

## 4. B 类（14 份）与仪器/实验原始输出

**B 类（14 份）不是语义差异**：唯一差异是 `extern 条数 1 vs 0`，且**段哈希/页/`sim_cycle` 全程全等**
（我复算：n_diffs = 1，只有 `extern_n`）。参考产物 tick 1 的 extern 是它自己的分配器要内存：

```text
063 ref extern[0] = {"name":"VirtualAlloc","args":[70408744189952,16384,4096,4],"ret":70408744189952}
063 ours extern   = []      （064 同形；042/094/133/135/137 也有这条，但它们另有段差）
```

⇒ 「只差 extern」时**不要**当成内存语义差异（与我方产物无关）。这条早先在 `status.md` 里已有指针。

**深挖用的对照实验（改的是 `work/tmp/diffclass/probe/` 下的副本，产品源码一字未动）**：

```text
# 仪器自检：当前 build/simplexc_cli.exe 重编 008.si（未改）→ blob 与 work/tmp/m5/out/008.bin
#   只在 0x590..0x5B5 差 24 字节；3 tick 宿主比对 = []（行为逐 tick 全等）⇒ 可当同一条仪器
# 仪器是否吃源码：把 008 无守卫块里的 `U1 ((U1 vid258))` 改成 `U1 ((U1 vid256))`
#   → blob 变（be6df0683df8b51d…）        ⇒ 吃源码
# 去掉 `if in_scope("check_output")` 守卫（块内语句保留）
#   → blob **不变**（7a16e0b99dd2de14…）  ⇒ 这条 in_scope 我们判真，守卫是空操作（老根因确实修掉了）
# 删掉整个守卫块（3 行）
#   → blob 变（819b5911debbdca3…，entry 0x570 vs 0x560）⇒ 块本身有码
# check_output 体末尾补 `return pass`          → blob **不变**
# check_output 体里 `output.output`→`level_output.output` → blob **不变**
```

最后两条**没分辨出 A 类的机理**（函数体这一层改动产不出字节差），所以 A 的根因写成
「`check_output` 的 fail 分支先触发」是**观测级结论**，机理是「返回值/字段读取」两种仍未排除，见 §5。

## 5. 遗留风险

1. **A 类只逐字节验了 010/024 两份**（008 用快照值复算过），其余 10 份是按「首段 + 差异字段集合 + 同形源码」
   外推的；要收口就跑一遍 A 类修正后的产物差分。
2. **A 的机理未定死**：`#CORRECT_OUTPUT[cycle]` 的比较 vs `handle_test_result` 读 `check_output` 的返回值，
   两条路都能解释观测。上面两条「blob 不变」是**异常线索**（体改动不进产物），下一轮应先看
   `--ir-out` 的 IR 再谈代码；`lower/` 有另一个写者，看代码前先对齐它当前的状态。
3. **C 类只到字节级**（±1），语句级定位（`get_input_switched` 那段）是推测。
4. **B 类的判据建议**：口径里把「只差 extern」单列（`extern_n` 且段/页全等），不要算进「差分回退率」；
   否则 14 份永远挂在「值不对」上。这是口径问题，得由主程定，本报告只给证据。
5. **E 类（9 份 ours 自己崩）没深挖**：崩点散在 `产物 +0xAC4…+0x9F31`、`params=[1, <大地址>]`
   （往一个很大的地址写 1）。早先诊断里这一档是**参考**产物崩（宿主 `VirtualAlloc` 桩语义，已修，
   见 `2026-10-18-host-va-fix.md`），现在崩的是**我们** ⇒ 是我们的产物自己的指针问题，属于第二层缺口。

## 6. 复现

```bash
python work/tmp/diffclass/scan.py        # 粗分类（只读 m5 日志，不跑语料）：scan.log / scan.json
# 字节级（4 份代表，各两跑）
for id in 010 024 110 042; do for s in ref ours; do \
  python work/tmp/tickdiff/probe.py $id $s 1 --dump-regions work/tmp/diffclass/$id/reg.$s; done; done
for id in 010 024 110 042; do python work/tmp/tickdiff/diffreg.py $id 1 \
  work/tmp/diffclass/$id/reg.ref work/tmp/diffclass/$id/reg.ours; done
# 实验（副本在 work/tmp/diffclass/probe/，参考侧用 work/tmp/ul14/refcc.py）
build/simplexc_cli.exe --src work/tmp/diffclass/probe/008_noguard.si --out work/tmp/diffclass/probe/out.bin
python simplexc/tools/host/host.py --src <si> --bin <blob> --entry 0x560 --ticks 3 --seed-reset-data seq --out x.jsonl --quiet
```

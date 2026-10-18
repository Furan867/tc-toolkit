# A 类「测试结果链」的机理：数组字面量缺 8 字节 Nim 头

日期 2026-10-18 · 实现者（诊断 + 修 `simplexc/lower/lower.cc` 一处）· 器材 `work/tmp/testchain/`
（不入 git；本文自带原始输出）。结论先行，过程与原始输出在后。

## 0. 结论（四点）

1. **异常线索是假警报 —— `check_output` 的函数体没被丢。** 那两条「改体不产字节差」的实验
   都是**语义空操作**：① 体末尾补 `return pass` —— 体本来就是 fall-through 到 `ret i64 0`，
   而 `Enum[pass, win, fail]` 的 pass = 0；② `output.output` → `level_output.output` ——
   调用点本来就是 `load @level_output` 后按值传参，O2 内联后两者是同一个值。IR 里那个体
   一字不缺（`mode_run$check_output#L428`，三个 `if` + fall-through `ret i64 0`）。
   **丢的不是函数体，是「对 `#CORRECT_OUTPUT.len()` 的判定」。**
2. **真机理**：`array_literal` 造数组字面量时**没带载荷前的 8 字节 Nim 头**（`p[-8] = len << 16`），
   而 `.len()` 读的就是 `u64(p-8) >> 16`。于是那条 `load` 是**越界的常量 load**，O2 里它是
   **undef** ⇒ `if cycle == #CORRECT_OUTPUT.len() - 1` 成了「对 undef 分支」⇒ LLVM 把
   **整条 `if` 连分支一起折掉**，`check_output` 退化成恒定 `ret 2`（= fail）。
   ⇒ `handle_test_result` 里 `halt()` ⇒ 本轮不再 `cycle += 1` ⇒ `sim_cycle` 恒落后 1、
   `input_replay[0]` 停在 0、`SIM_STATE+0x100/101` 跟着 0。
3. **修法**（已改、已编译，**未提交**）：`array_literal` 造 `{ [8 x i8] 头, [n x el] 载荷 }`
   聚合常量、返回 GEP 到字段 1 —— 与 `builtin_array`/`empty_array` 已有的口径一致。
4. **13 份现状**：9 份全等（008/010/012/014/016/018/022/024/056）· 096 只剩 `extern 条数 1 vs 0`
   （B 类口径，不是语义差）· 仍 diff 3 份（060/061/098，见 §4）。

## 1. 异常线索为什么不产字节差（复核 + 解释）

复核（与 `diff-classify.md` §4 同一条仪器）：`build/simplexc_cli.exe` 编 008 →
`7a16e0b99dd2de1478a8aa6e28b3e9bf`（与诊断一致，仪器可信）。

- `#CORRECT_OUTPUT = [U1 1,0]` 在 `run_sim` 块里，`check_output` 是同一块里的**嵌套 def**、
  调用点只有一处（008.si:347-350，`if in_scope("check_output")` 守卫我们判真 ⇒ 空操作）。
- `--ir-out` 里 `check_output` 的**整个体在**，且 O2 后**内联进 `mode_run`**。两个实验改的就是
  这个体，改完语义不变，所以机器码不变 —— 线索到此为止，不用再怀疑「体被丢」。

## 2. 机理：怎么定死的（探针 + 反汇编）

器材：`work/tmp/testchain/probe_src.py`（编译任意 `.si` → 用 `host.py` 跑 N tick → 读指定地址）、
`make_dbg.py`（往 008.si 副本里插 `store(settings + 0x3xx, …)` 探针）。
`settings = 0x232A7000000`，探针写在 `settings + 0x300…` 这一段（不被任何消费者读）。

tick 1（cycle = −1，即第 1 轮）原始输出（u64，偏移 0x300 起）：

```text
+0x300 cycle(轮首)      = -1        +0x338 #CORRECT_OUTPUT[0] = 1
+0x308 level_input.input= 0         +0x340 #CORRECT_OUTPUT[1] = 0
+0x310 level_output.out = 1         +0x348 #CORRECT_OUTPUT.len() = 1   ← 假值（应为 2）
+0x318 result           = 2 (fail)  +0x350 input.input = 0
+0x320 check 的 cycle   = 0         +0x380 动态下标 #CORRECT_OUTPUT[cycle] = 1
+0x328 check 看的 output= 1         +0x388 (dyn == output) = 1
+0x330 output_is_z      = 0         +0x390 (dyn != output) = 0
+0x358/0x360/0x368 = 0/1/0  ← 第 1 个 if 没进、**第 2 个 if 进了**、win 没进
```

`dyn`（动态下标，LLVM 折不掉的那个 load）实测 = 1，`output.output` = 1，**同一个表达式**在探针里
算出来 `!=` 是 **0**，可 `if #CORRECT_OUTPUT[cycle] != output.output` 的体**照样执行了**。

`.len()` 的值随源码行号漂移而变（另一次构建里同一条探针读到 **0**）⇒ 它读的是载荷前一块无关数据，
不是任何「算错」—— 直接印证「缺头」。

反汇编（`--obj-out` + `objdump -D`，O2 后的 `tc_main`）：

```text
3c6: cmp  %al,%cl          ; arr[cycle] vs output.output（运行时 1 vs 1）
3c8: sete %dl              ; 探针的 eq = 1
3cb: setne %r8b            ; 探针的 ne = 0
3cf: mov  %rdx,+0x388      ; …
3d6: mov  %r8, +0x390      ; …
3e4: movq $0x1,+0x360      ; 第 2 个 if 的体 —— **没有任何分支指令**
3ef: movq $0x2,+0x318      ; 函数返回值被折成常量 2（fail）
```

⇒ **没有那条 `br`**：LLVM 有 undef 在手，直接选了「进 fail」这一支，另一支（含 win 那条
`cycle == len-1`）成了不可达代码。这也解释了为什么 3 份之外的行为全对得上：`sim_test_result`
恒 2、`sim_cycle` 每 tick 少 1、`input_replay[0]` 停在 0。

**对照实验（决定性）**：把 008 副本里的 `#CORRECT_OUTPUT.len() - 1` 换成 `1`（等于 `.len()` 正确时
的值，但**消掉了那条越界 load**）→ 我们的产物在 tick 1 给出
`sim_cycle=1 · sim_test_result=1(win) · input_replay[0]=1` = **参考值逐项相同**。

## 3. 修法与现状（未提交）

`simplexc/lower/lower.cc` 的 `array_literal`：改成
`{ [8 x i8] 头, [n x el] 载荷 }` 的私有常量全局，返回 `GEP{0,1}`（= 载荷首址）。
头 = `n << 16`（低 16 位 log2 容量取 0，与 `array(<T>, n)` / `empty_array` 同口径）。

- **能编译**：`python simplexc/tools/build.py simplexc_cli` 成功；编辑工具报 pi-lens clean。
- **A 类 13 份**（`work/tmp/testchain/coverage_A.tsv`，`--report/--tsv/--summary-json/--out-dir`
  都指到 `work/tmp/testchain/`）：
  `总 13 · 可装配 13 · 全等 9 · diff 4`（改前 0 全等）。全等的 9 份：
  008 010 012 014 016 018 022 024 056。
- 096：唯一差异 = `extern 条数 1 vs 0`（B 类，不是语义差）。
- **未做的验收**：全量 98 份的回归跑（确认 21 份全等不掉）· `check.py` 5 PASS · 提交 · 账本折叠。

## 4. 遗留（060/061/098）

`060.si:1634` 的 `check_output` **不用 `.len()`**（用 `cycle == 0xff`），`asr`/`get_binary_repr`
是渲染路径；061 同形。098 的 ours 段哈希与其余 11 份**不同**（`8d71644220de0d7e…`）。
⇒ 这 3 份是**另一条**机理，本轮没查；先按「与 A 无关」处理，别把这次的头修法当万能。

## 5. 复现

```bash
python simplexc/tools/build.py simplexc_cli
python work/tmp/testchain/probe_src.py simplexc/corpus/src/008.si work/tmp/testchain/p/008.bin 3 \
    0x232a7000000:0x20 0x232a7010000:0x10        # 修后：sim_cycle=1 sim_test_result=1
python work/tmp/testchain/make_dbg.py  simplexc/corpus/src/008.si work/tmp/testchain/p/dbg.si
python work/tmp/testchain/make_dbg2.py work/tmp/testchain/p/dbg.si work/tmp/testchain/p/dbg2.si
build/simplexc_cli.exe --src work/tmp/testchain/p/dbg2.si --out work/tmp/testchain/p/dbg2.bin \
    --obj-out work/tmp/testchain/p/dbg2.obj && objdump -D work/tmp/testchain/p/dbg2.obj | less
python simplexc/tools/m5/coverage.py --ids 008,010,012,014,016,018,022,024,056,060,061,096,098 \
    --tsv work/tmp/testchain/coverage_A.tsv --summary-json work/tmp/testchain/coverage_A.json \
    --report work/tmp/testchain/coverage_A.md --out-dir work/tmp/testchain
```

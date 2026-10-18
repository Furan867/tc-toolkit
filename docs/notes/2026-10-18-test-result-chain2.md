# 测试结果链剩下的 3 份：060/061 修好（`+` 的字面量位宽），098 归到另一个缺口

日期 2026-10-18 · 实现者（诊断 + 修 `simplexc/lower/` 三处）· 器材 `work/tmp/testchain2/`
（不入 git；本文自带原始输出）。结论先行，原始输出在后。

## 0. 结论

1. **060 / 061 已修**（同一机理）：`check_output` 的守卫是
   `if output.output != (~input.input + 1)`，`input.input` 是 `U8`。参考把 `~x + 1` 算在
   **8 位**上（x == 0 得 0 回绕），我们按 `unify` 的「宽者胜」算成 **64 位的 256** ⇒ 第 1 轮
   （cycle = −1、input = 0、output = 0）就 `return fail` ⇒ `handle_test_result` 里 `halt()`
   ⇒ 与 A 类（数组头）**同样的可观测量**：`sim_cycle` 恒落后 1、`sim_test_result` 恒 2、
   `input_replay[0]` 停在 0。两轮**存**的差别只在那一处比较。
2. **修法**：`lower.cc` 加 `arith_ty()` —— 字面量多态：`+` 的两侧一宽一窄时，取**窄**的那一侧。
   **只给 `+`**：同一条规则套到 `%`/`<<`/`/` 上会让 176/177/180/rv32im 从全等掉成 diff（见 §3）。
3. **098 不是这条机理**：它的首个不同语义点已经缩到「全局数组的写没生效」
   —— `registers[0] = instruction & 0b111111` 之后**紧接着读回** `registers[0]` 仍是 0
   （case 体确实执行了，见 §4）。这是**另一个缺口**，本轮没修（按停止规则只分类列出）。

## 1. 060/061：机理定死的原始输出（参考 vs 我们，同一份带探针的 060.si）

探针插在 `check_output` 体**首**：`store(settings + 0x3xx + (U64 cycle) * 8, …)`，
`settings = 0x232a7000000`（该段无消费者）。**同一份源码**分别用参考编译器
（`work/tmp/testchain2/probe_ref.py`，内部用 `work/tmp/ul14/refcc.py`）与我们的 CLI 跑 1 tick：

```text
参考（tick 1 结束 sim_cycle=1 sim_test_result=0）：
  +0x380 [cycle=0] output.output = 0        ← 第 1 轮（cycle 参数 0）也在跑
  +0x380 [cycle=1] output.output = 0xf6
  +0x400 [cycle=0] input.input   = 0
  +0x400 [cycle=1] input.input   = 0x0a
  +0x480 [cycle=0] ~input.input+1 = 0       ← 关键：x == 0 时是 0，不是 256
  +0x480 [cycle=1] ~input.input+1 = 0xf6
  +0x600 [cycle=0..2] = 0x5e57e0… 哨兵（证明 cycle=0 那次**确实调过**，不是「没写」）
  +0x400 [cycle=0] 的 `1 + ~input.input` 也是 0  ⇒ 不是「左操作数胜」，两侧对称

我们（tick 1 结束 sim_cycle=0 sim_test_result=2）：
  +0x300 output.output = 0 · +0x308 input.input = 0 · +0x318 cycle = 0
  +0x310 ~input.input+1 = 0x100 (= 256) · +0x320 U8(~input.input+1) = 0
  ⇒ `256 != 0` 判真 ⇒ return fail（第 1 轮，cycle = −1）
```

`--ir-out` 里同一处也直接看得到（O2 前）：

```llvm
%bnot = xor i64 %m12, 255
%m13  = and i64 %bnot, 255
%op   = add i64 %m13, 1        ; ← 没有截回 8 位：字面量 1 把结果抬成了 64 位
%cmp  = icmp ne i64 %m9, %op
```

## 2. 修法与判据（原始输出）

`lower.hh`/`lower.cc`：`arith_ty(a, b, op, line)` —— `op == "+"` 且两侧整型、位宽不同、
宽的那一侧是**常量**（`pat_const` 有值 = 字面量/常量折叠结果）时，返回窄的那一侧，否则退回 `unify`。
`binop` 的 `unify(a.ty, b.ty, line)` 改成 `arith_ty(a, b, op, line)`（其余 op 一字未动）。

```text
$ python simplexc/tools/m5/coverage.py --ids 060,061,098 --out-dir work/tmp/testchain2（改前）
  总 3 · 可装配 3 · 逐 tick 全等 0 · diff 3
  [1/3] 060 diff diff=10 · tick 1: 段 0x232a6ff0000 A=b7fb9169… / B=14ddcdb3…
  改后（work/tmp/testchain2/coverage_fix1.tsv）：
  [1/3] corpus/060 extern_only diff=1 · ExternOnly: extern ref 2 条（VirtualAlloc×2）/ ours 0 条
  [2/3] corpus/061 extern_only diff=1 · 同上
  060/061 的段/页**逐 tick 全等**，唯一差异是参考产物自己调了 2 次 VirtualAlloc（= 已有的 B 类口径）
```

全量回归（`work/tmp/testchain2/coverage_final.tsv` / `.log`，最终二进制）：

| （98 份语料） | 全等 | 只差 extern | diff | cli_fail |
| --- | --- | --- | --- | --- |
| `work/tmp/m5/coverage.tsv`（数组头修复**前**，没有 extern_only 这一档） | 21 | — | 54 | 23 |
| 数组头修复后 + 单列 extern_only（= 上一轮的账：30/21/24/23） | 30 | 21 | 24 | 23 |
| **本次改后** `work/tmp/testchain2/coverage_final.tsv` | **30** | **23**（+060/061） | **22**（−060/061） | 23 |

`baseline/rv32im` 亦为 `equal`。`python simplexc/tools/check.py` → **5 步 5 PASS · 0 FAIL · 0 SKIP**。
pi-lens 命中 0：`ast-grep scan --config work/tmp/lens2/sgconfig_all.yml --json=stream
simplexc/lower/lower.cc simplexc/lower/lower.hh` → 0 条；同一入口扫一份含 `printf` 的 .cc → 命中
`cpp-no-printf`（入口是活的）。

## 3. 踩到的坑：不能把「字面量取窄位宽」铺给所有运算符

第一版把规则铺给所有算术 op ⇒ 176/177/180/rv32im（rv32im 是强判据那份）**从全等变 diff**：

```text
改（全 op）：总 98 · 全等 27 · 只差 extern 22 · diff 26
  176 equal -> diff（4 tick）· 177 equal -> diff（2 tick）· 180 equal -> diff（4 tick）· rv32im 同
收窄到只给 `+`：176/177/180 全等回来，060/061 仍修好
```

⇒ **参考的字面量位宽规则按 op 不同**（`+` 已验；`%`/`<<`/`/` 上按窄位宽算与参考不符，反例就是
上面那四个）。这条**没进账本**，等有人逐 op 验过再写 `facts.md`。

## 4. 098：首个不同语义点（已定位，未修）

搜索引擎与 060 同：`work/tmp/testchain2/probe_ref.py`（参考）+ `work/tmp/testchain/probe_src.py`（我们）。
098 的 `check_output` 在 `cycle` 索引上打「哪个出口被走到」的标记：

```text
我们 tick 1：+0x300 = 0xd4  → 出口 = `if registers[reg_i] != value`（寄存器比对）
  逐 reg_i 的值：+0x380 [reg_i=0] registers[0] = 0
                +0x400 [reg_i=0] get_memory("Reg 0") = 0x11
  参考 tick 2：+0x380 [reg_i=0] registers[0] = 2 · +0x400 [reg_i=0] = 2（相等，继续往下走）
```

再往下切：`switch opcode << 6 / case #IMM { registers[0] = instruction & 0b111111 }` —— 三个探针
（一个在赋值**前**、一个在赋值**后**、一个写在 `var reg_i = 0` 处）一次跑出来：

```text
+0x738 = 0xbbbb（case 体进来了）· +0x730 = registers[0] 赋值后**立刻读回** = 0
+0x700 = registers[0]（switch 之后）= 0 · +0x710 = instruction & 0b111111 = 0x11
```

⇒ **case 体执行了，但 `registers[0] = …` 没生效**（`var registers = [0,0,0,0,0,0]` 是顶层全局、
数组字面量初始化；参考在同一处读到 2）。这是**数组字面量初始化过的全局数组的读写**这条缺口，
与 060/061 的 `+` 位宽、与 A 类的「数组头」都不是同一处 —— 按停止规则本轮只分类，不动手。

## 5. 复现

```bash
python simplexc/tools/build.py simplexc_cli
# 060/061 机理（同一份探针源码跑两侧）
python work/tmp/testchain2/probe_ref.py     work/tmp/testchain2/p/060_sent.si 2 0x232a7000600:0x30 …
python work/tmp/testchain/probe_src.py      work/tmp/testchain2/p/060_probe.si out.bin 3 …
# 098 的三个探针
python work/tmp/testchain/probe_src.py      work/tmp/testchain2/p/098_arr.si out.bin 1 0x232a7000700:0x38
python work/tmp/testchain/probe_src.py      work/tmp/testchain2/p/098_case.si out.bin 1 0x232a7000730:0x10
# 回归（--report/--tsv/--out-dir 都指到 work/tmp/testchain2/，不覆盖仓库那份报告）
python simplexc/tools/m5/coverage.py --tsv work/tmp/testchain2/coverage_final.tsv \
  --summary-json work/tmp/testchain2/coverage_final.json \
  --report work/tmp/testchain2/coverage_final.md --out-dir work/tmp/testchain2
python simplexc/tools/check.py
```

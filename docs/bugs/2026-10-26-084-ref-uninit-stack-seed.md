# 084（`campaign/overture_1_registers`）：参考把随机种子读成未初始化的栈值

- 状态：**不修；不是我们的 bug** —— 参考侧不可复现（README 的「修好之后」不适用：这条永远不会因为改我们的代码而变绿）
- 影响：084（关卡目录 `campaign/overture_1_registers`，官方中文名未核实；定关依据：源码里
  `ui_set_instruction(0, #MOV | instruction, cycle)` 与该关测试代码逐字相同，全 campaign 只此一处，
  见 `grep -rl '#MOV | instruction' simplexc/corpus/campaign/`）
- 现象：100 tick 后终态差 **12 字节**（`#SIMULATION_STATE + 334..345` = Reg0..Reg5 与它们的 cache 副本），
  首个不同在 tick 16（0 基 15）、1 字节

## 已确认

- **抽取序号相同**：两侧 `seedscan.py` 都给「base=14、步长 8」（第 15/23/31/39/47/55 次抽取写进寄存器）。
  差的不是次数，是**种子**。
- **我们的种子 = 源码值**：`Seed 1` → `d1 8c 42 a6`；`Seed 42` → `4a 54 66 18`（= xorshift(42)）。
  实现是 `$x` 真指针（`sema/analyze.cc:1653` 实参降左值；`codegen/codegen.cc:1220` 传地址、`1180` 形参即指针），
  种子的 64 位 load/store 在 `codegen/codegen.cc:1478`。
- **参考的种子与源码无关**：源码写 `Seed 42` 时读回全局确实是 `0x2a`，链却仍按 `xorshift(7)` 走；
  每 tick 重设种子也进不去链（我们 `d1 d1 d1 d1`，参考 `b7 d5 69 d8` 继续走）；`--seed seq|none|xorshift` 都一样。
- **反汇编证据**：参考的链住在栈槽，取用与写回都是同一槽（`code+0x6658` / `+0x9a6e`），
  而同 tick 更早的无关代码（时间/节流那段，`code+0x4321`）先往该槽写了 `7`。
- 数值指纹：我们的 `4a c7 c9 3e 91 b7` 唯一命中 `seed=1`；参考的 `73 88 d4 24 19 5e` 唯一命中 `seed=7`。

## 已排除

- **「我们的 random 实现不同」**：同一 seed 下两侧逐位相同（`work/tmp/rc084/` 的负对照；与 `docs/facts/semantics.md` 的 `random` 节一致）。
- **「抽取次数不同」**：见上（序号相同）。
- **「宿主种子不同」**：`--seed seq|none|xorshift` 三种都一样。
- **「与 096/098 同机制」**：不同 —— 096/098 是**种子被降成 1 字节**+未初始化读（`docs/bugs/2026-10-25-random-stream-offset.md`）；
  084 是**同一 64 位链、种子被换掉**。共同点只是「参考侧读了未初始化的栈」。

## 判据

不纳入 M5 的 equal 分母，标 `known-ref-unreproducible`。理由：判据要求「同一源码逐 tick 等价」，
而参考在这份上换一个栈初值就换一条序列 —— 它自己也不可复现。

## 下一步

不修。若将来一定要判 equal，唯一办法是在宿主里把那个栈槽预置成 `7`（模拟参考的未初始化读）；
那是**判据决策**，不是修 bug。

## 复现

```bash
build/difftrace.exe diff --state work/tmp/rc084/seq/084.ref.seq work/tmp/rc084/seq/084.ours.seq \
    --src simplexc/corpus/src/084.si --tick-mark 0x232A7010000
```
两侧 `.seq` 的生成命令、探针 `084probe*.si` 与完整命令清单见 `work/tmp/rc084/report.md`（临时目录，会被清）。

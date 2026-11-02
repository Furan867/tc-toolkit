# 084「Overture 1 寄存器序章」（`campaign/overture_1_registers`，本地译名）：两侧随机序列不同（原因待查）

- 状态：**差异存在，原因待查；差分测试暂不可复现** —— 同一份源码两侧走出的随机序列不同，根因未定论
- 影响：084「Overture 1 寄存器序章」（关卡目录 `campaign/overture_1_registers`；**中文名是本地译名，不是官方名**）。
  定关依据：源码里 `ui_set_instruction(0, #MOV | instruction, cycle)` 与该关测试代码逐字相同，全 campaign 只此一处，
  见 `grep -rl '#MOV | instruction' simplexc/corpus/campaign/`
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

**没有**判据结论：差异存在，原因待查。已知的只是两侧现象不同（我们的种子跟随源码，参考的链从栈槽取输入），
这不构成「谁对谁错」的结论。

## 下一步

查参考为何从栈槽取种子，以及我们的实参降级与种子存放是否有与参考不一致之处。
查清之前这份只记为「差异」，不进 equal 分母，也不定性为谁的 bug。

## 复现

```bash
# ① 两侧各编一次，存下逐 tick 的 seq
python simplexc/tools/m5/batch.py --with-prefix --ids 084 --ticks 100 \
    --out-dir work/tmp/rc084 --tsv work/tmp/rc084/batch.tsv --report work/tmp/rc084/batch.md
# → 首址 0x100018E0，diff_game=2334 字节

# ② 只比游戏状态区（[game_lo, game_hi)）
simplexc/build/difftrace.exe diff --state work/tmp/rc084/seq/084.ref.seq \
    work/tmp/rc084/seq/084.ours.seq --src simplexc/corpus/src/084.si --tick-mark 0x232A7010000
```

判据值：终态 12 字节不同，全在 `#SIMULATION_STATE + 334..345`（`Reg0..Reg5` 与它们的 cache 副本），
首个不同在 tick 16（0 基 15）、1 字节。`ours` 的 6 字节 `4a c7 c9 3e 91 b7` 唯一命中 `xorshift(seed=1)`；
`ref` 的 `73 88 d4 24 19 5e` 唯一命中 `xorshift(seed=7)`。两侧抽取序号相同（第 15/23/31/39/47/55 次写进寄存器）。

数据（`.seq` / `.bin` / 探针 `.si`）是现场生成的，不进仓库；上面的命令能重建。

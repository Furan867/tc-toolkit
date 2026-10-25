# 096/098 的随机流错位（相差 34 次抽取）

- 状态：**未修**
- 影响：`simplexc/corpus/src/096.si`、`098.si`（两个同族电路：8 位标量 CPU，098 = 096 加 `IMM`）。
  98 份语料里只有这两份还有真差异（100/102 的已随「局部数组字面量只读」修掉）。
- 现象：100 tick 后终态差 **12 / 11 字节**，全在 `#SIMULATION_STATE + 406..421`（6 个电路寄存器
  与它们的 cache 副本）；`error_buffer` 两边都空、`sim_cycle` 都跑满。
- 判据：`batch.py --ids 096,098` 两份变 `equal`（`#SIMULATION_STATE` 内 0 差异）。

## 已确认（都有出处）

- **boot 后游戏区两边完全一致**：`--dump-boot` 在 boot 结束、发 `run` 命令前拍 `[game_lo, game_hi)`，
  62 MB 区间差异 **0 字节**，`#SIMULATION_STATE + 0..512` 也是 0。所以差异产生在 run 阶段。
  → `19d0529`（`--dump-boot` 本身）
- **首次不一致**：`diff --state --tick-mark 0x232A9F501A5` 报 `tick 数 A 102 / B 102`、
  `第一个不同 : tick 36（0 基，即第 37 个 tick），差异 1 字节`、
  `addr=0x232a9f501a0`（= `#SIMULATION_STATE + 416`），**A=0xc7 / B=0xa5**。
- **两个值在同一条 xorshift 流上，相差 34 次抽取**：以 `state=1` 起算，
  `0xc7` 是**下标 33**（第 34 个），`0xa5` 是**下标 67**（第 68 个）。
- **基础机制没问题**（负对照）：`work/tmp/minrepro/min.si`（97 行：协议区 + 每 tick
  `store(#SIMULATION_STATE + 0, U8 get_input(cycle+1).input)`）两边各跑 50 tick，
  `+0` 的 51 次写**逐位相同**，且都等于 `xorshift(1)` 的低字节序列；
  再补上 098 的 32 次 `U8 random(64)` 预抽（`min_pre32.si`）后，两边仍都等于
  `xorshift[32:83]` —— **预抽次数也一致**。
- tick 37 是这个电路**第一次把 `input` 写进寄存器**（`MOVE Reg3 ← input`）；前 36 tick
  的指令都不读输入，所以「流从哪个 tick 开始错」在 tick 37 之前不可观测。

## 已排除（别重走）

- **「参考不写回 `global_seed`」**：参考把 `internal global` 提升到栈/寄存器（SROA），
  内存里只是看不见；6 份用 `random` 的语料（034/065/069/086/088/102）全都能在栈上
  看到 xorshift 链条。写回位置不是判据。
- **「`instructions[]` 保持初值」**：曾把参考侧 `0x400BA43BB008` 的 1212 次写当成
  `instructions[]`，实际是 1 字节 ASCII（`'R','e','g',' ','0'` + 每 tick 递增的数字），
  是 `ui_set_text` 复用的 Nim 字符串缓冲。
- **「`array()` 的私有全局是根因」**：改成运行时分配（`19d0529`）后 098 的分叉位置
  **一个都没变**——098 每轮把读到的元素全重写一遍，旧实现本来就等价。
- **「预抽次数不同」**：`min_pre32.si` 两边预抽次数相同（见上）。
- **「随机数发生器实现不同」**：`min.si` 两边序列逐位相同、且都等于理论 xorshift。

## 下一步

从 `min.si` 出发**增量逼近**，每加一段 098 的结构就跑一次两边、比 `+0` 的序列：

1. `instructions[]` 表（`array(<U8>,256)` + 32 次 `random(64)` + ALU/mov 常量段）；
2. `get_input` 改用 `instructions[cycle]`；
3. `check_output`（`registers[6]` 软模型 + `get_memory("Reg N")` 比对）；
4. `mode_refresh`（098 的 461 字节状态写）。

哪一步之后出现「相差 34 次」，就是它。若四步全加完仍一致，说明差异在更细的组件行为
（`com_*` 的 LATE / `halt` / burst 节流），那时把「已排除」的清单再往下扩。

## 复现

```bash
build/simplexc_cli.exe --src simplexc/corpus/src/098.si --out <blob>
build/difftrace.exe run --src simplexc/corpus/src/098.si --bin simplexc/corpus/ref/098.bin \
    --target 100 --seed seq --save ref.seq
build/difftrace.exe run --src simplexc/corpus/src/098.si --bin <blob> \
    --target 100 --seed seq --save ours.seq
build/difftrace.exe diff --state ref.seq ours.seq --src simplexc/corpus/src/098.si \
    --tick-mark 0x232A9F501A5
```

负对照在 `work/tmp/minrepro/`（`min.si`、`min_pre32.si`、`tcref.py`）。

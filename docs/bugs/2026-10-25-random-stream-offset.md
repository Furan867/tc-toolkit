# 096/098：参考把随机种子降成 1 字节栈槽（不是「相差 34 次抽取」）

- 状态：**不修；不是我们的 bug** —— 参考侧不可复现
- 影响：096（关卡目录 `campaign/overture_2_alu`）、098（`campaign/overture_3_immediates`）。
  定关依据（两条，都是逐字比对测试代码）：
  ① `instructions[i] = …` 的生成序列 —— 096 与 `overture_2_alu` 同序（首行 `#MOVE | #INPUT | 1`），
  098 与 `overture_3_immediates` 同序（首行 `#IMM | U8 random(64)`）；
  ② `#IMM` 出现次数 096=1（只在常量定义）、098=4（定义 + 生成 + 两处 `case`），与这两关的 1 / 4 对上；
  `overture_4_program` 是 2 次，排除。
- 现象：100 tick 后终态差 12 / 11 字节，全在 `#SIMULATION_STATE + 406..421`（6 个电路寄存器与 cache 副本）；
  两边 `error_buffer` 都空、`sim_cycle` 都跑满

## 已确认（本文件 2026-10-26 订正，替换掉原来「同一条流相差 34 次」的结论）

- **两侧 `random` 调用次数完全相同**：101 次，实参 `0x00..0x64` 逐条一致（在 `get_input` 入口挂只读观察者）。
  差的不是抽多抽少，是**两条不同的递推**。
- **参考侧把种子降成 1 字节栈槽**：反汇编里链的输入是 `movzx rcx, byte ptr [rsp-0x8b]`、
  写回是 `mov byte ptr [rsp-0x8b], cl`（ref blob `code+0x66fa` / `+0x6799`）；
  64 位的 `global_seed` 全局在 run 阶段**一次都没被写**（101 次读全是 32 次预抽后的值）。
- tick 1 的那个字节是**未初始化的 `0x07`**，之后每 tick 只保留低字节 → 纯乘链、周期 16：
  `b7 67 17 c7 77 27 d7 87 …`。
- **我们侧是对的**：`xorshift(state=1)` 从下标 32 起，`6b c7 05 d9 06 56 …`
  （`codegen/codegen.cc:1478 builtin_random` 对整 64 位做 load/store）。
- 原文档的两个数值是**巧合**：`0xc7` 是那条退化链的第 4 个值，`0xa5` 是正确流的下标 67。
- 四步增量：`m2.si`（instructions[] + 32 次预抽）两侧逐位相同；`m3.si`（+`instructions[cycle]` + U8 局部 + switch）
  仍逐位相同；完整 098 加只读观察者后，**第 0 个写（tick 1）就分道**：ref=`0xb7` vs ours=`0x6b`。
- 删除式二分：与 `check_output`、`mode_refresh`、`get_input` 的 switch/局部、sleep-ahead 循环、burst 节流、
  `#if in_scope` 包裹、预抽次数（64 次）、种子初值（1001）、`array<U8>`→`array<U64>` **全都无关**（删了照样退化）。
- 096 的参考产物同样退化、首值同为 `0xb7` —— 同族。

## 已排除（保留上一轮的死路清单）

- **「参考不写回 `global_seed`」**：参考把 internal global 提升到栈/寄存器（SROA），内存里看不见；写回位置不是判据。
- **「`instructions[]` 保持初值」**：曾把参考侧 1212 次写当成 `instructions[]`，实际是 `ui_set_text` 复用的 Nim 字符串缓冲。
- **「`array()` 的私有全局是根因」**：改成运行时分配后分叉位置一个都没变。
- **「预抽次数不同」**：两侧相同。
- **「随机数发生器实现不同」**：同 seed 下逐位相同、且都等于理论 xorshift。

## 未闭环（不影响归类）

参考**为什么**在 096/098 里把这个种子降成 1 字节栈槽（`m2`/`m3` 这种小片段复现不出来）。
但那是参考产物自己的类型/布局选择 + 未初始化读，与我们的 lowering 无关；要继续追就从 098 的标架删
（`reset_sim` 内容、`registers`/`tests`、嵌套 `halt`/`handle_test_result`/`get_target_cycle`、`level_input` 全局）。

## 判据

这两份不纳入 M5 的 equal 分母，标 `known-ref-unreproducible`。

## 复现

```bash
./work/tmp/rc096/repro.sh          # 输出 work/tmp/rc096/repro.out
./work/tmp/rc096/ladder.sh <name>  # 单个阶梯用例的两侧对照；二分表见 work/tmp/rc096/bisect.out
```

（`work/tmp/` 是临时目录，会被清；要重跑就按上面的脚本重建。）

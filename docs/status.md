<!-- 整体重写，禁止追加。≤80 行。
     一次性结论 → notes/；被复用过的结论 → facts.md。

     写法：
     - 「下一步」只放**已经决定要做**的；还没定的写成「待定：…」，
       不要把提案写成计划（那会替人做一个他还没做的决定）
     - 人的**局部取舍**要写明它的条件（「暂时…，因为…」），条件变了就重新考虑
-->
# 状态

在追：

## Simplex → LLVM 编译器（主线，2026-10-13 起）

目标：把游戏目录的 `compile.dll` 换成自研实现 —— Simplex → LLVM IR → 优化 → 装配成游戏可 JIT 的 blob，
提升游戏内电路仿真主频。正确性判据：同一份 `.si`，我们的产物与参考产物**逐 tick 状态等价**。
主频测量用 `watch_freq.py`，benchmark 用 RV32IM 电路（很远，暂不做）。

**设计定稿**：`docs/specs/2026-10-15-simplex-llvm-compiler-design.md`
（产物契约由 T1–T5 实测闭合：`flags` 恒 0、`out[3]/out[4]` 是错误消息字符串、
blob 头 bit62 决定游戏是否 free、entry = 顶层块且每次编译只进入 1 次。）

### 下一步（已决定）

1. **spike**：手写 LLVM IR → `-O2` → 我们的装配器 → 游戏里跑通一个空程序。
   一次验掉 DLL 形态、LLVM 版本、装配链路、头 bit62、entry 包装；不崩游戏即算通过。
   壳需要新增「选实现」开关（`ours` / `ours_with_fallback` / `reference`），失败一律整单转交
   `compile_real.dll` 并原样返回它的 Result。
2. **facts.md 补 4 条**：`flags` 恒 0（10/10 实测）；`out[3]/out[4]` = 错误消息 `{len,ptr}`（非 0 即失败，
   失败必须给非 0，否则游戏走成功路径触发 `raiseIndexError2`）；blob 头 bit62 = 不要 free
   （`eqdestroy` @`0x14001c260` 门控）；`entry` = 顶层语句块且每次编译只进入 1 次。
3. **观测设施**：语料固化（92 份 campaign test.si + 16 份生成源码）+ oracle CLI + 离线差分宿主
   （假游戏内存 + extern 桩 + 逐 tick 驱动）；先用「参考产物 vs 参考产物」自比对证明宿主可靠。
4. **前端与 runtime**：先走路线 B（自研 runtime + 只吃生成代码子集 + 失败回退），跑通后再上路线 A
   （全 stdlib，终态）。

### 待定

- 代码落库位置：建议 `reversing/simplexc/`（与语料/工具/文档同仓），未拍板。
- 判据入口（一条命令 + 覆盖率矩阵）、agent 接口冻结清单、Steam 更新后的按符号名重验脚本：
  设计已起草（spec §7.5 / §8 / §10），**待确认**。
- benchmark 协议与性能目标：暂缓（测法已定，见 spec §7.3）。

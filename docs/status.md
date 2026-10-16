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

**设计定稿**（2026-10-15 负责人确认，含后续补充）：
- 主设计 `docs/specs/2026-10-15-simplex-llvm-compiler-design.md`：产物契约由 T1–T5 实测闭合
  （`flags` 恒 0、`out[3]/out[4]` 是错误消息字符串、blob 头 bit62 决定游戏是否 free、entry = 顶层块
  且每次编译只进入 1 次）；§7.5 判据入口 / §8 agent 工作流 / §10 重验脚本已确认；
  代码落库 `reversing/simplexc/`；**LLVM 链接方式 = `LLVM-C.dll` + C API + 现有 w64devkit g++**
  （不引入 CMake，构建照抄 `tools/deploy_compile_hook.py` 的直调 gcc）。
- 主频测量层 `docs/specs/2026-10-15-freq-measurement-design.md`（5 项决策已确认）。
- **语料实测**：`work/hook/all/` 180 份源码 + 180 份参考产物 + `work/hook/manifest.tsv`
  → 去重 98 份唯一源码，**180 组「源码, 参考产物, entry」三元组**（oracle 不用跑游戏即可建）。
- **MVP 验收下界 = 沙盒模式能跑起来**；主线关卡专用构造允许「显式报错 + 回退」，用回退率量化。

### 下一步（已决定）

1. **spike**：手写 LLVM IR → `-O2` → 我们的装配器 → 游戏里跑通一个空程序。
   验收点：**`LLVM-C.dll` 能否被 MinGW 链接并调用**（C API：`LLVMRunPasses` +
   `LLVMTargetMachineEmitToMemoryBuffer`）、DLL 形态、装配链路、bit62 置 1 后游戏不 free、
   entry 包装（最外层必须是手写 shim：保存寄存器 + 对齐栈 16 字节）、不崩游戏。
   壳要新增「选实现」开关（`ours` / `ours_with_fallback` / `reference`），失败一律整单转交
   `compile_real.dll` 并原样返回它的 Result。
2. **M1.5 收益探针**：一块最小电路的端到端主频 A/B（参考产物 vs 我们的产物），先量出量级。
3. **观测设施**：语料固化（180 份 → 去重 98 份 + manifest + **按构造打标签**）+ oracle CLI +
   离线差分宿主（假游戏内存 + extern 桩 + 逐 tick 驱动）；先用「参考产物 vs 参考产物」自比对证明宿主可靠。
4. **前端与 runtime**：先走路线 B（自研 runtime + 只吃生成代码子集 + 失败回退），跑通后再上路线 A
   （全 stdlib，终态）。同一个核心要有**两个前端**：DLL 壳与离线 CLI（agent 日常走 CLI，不碰游戏）。
5. **判据入口**（spec §7.5）：一条命令跑「全语料编译 → 逐 tick 差分 → 覆盖率矩阵 + **回退率**」，
   机器可读报告 + 人读摘要；游戏内部分单独标 slow/manual。
6. **Steam 更新后的重验脚本**（spec §10 的 5 条断言）：按符号名定位，不硬编码地址。
7. **主频/bench 层重写**：设计见 freq spec。顺序：零行为变化抽库（用现有
   freq_log.tsv / freq_summary.txt / bench_*.tsv 回归）→ 统一 median → 泛化 `Variant(compiler, rewrite)`
   → JSON 报告接判据入口 → 收尾。附带转发层改动：`hook.log` 从 `work/opt/` 移到 `work/hook/`
   + 新增 per-call「实际实现」行（与 spike 同碰 `tools/compile_hook.c`，两条线**不要同时派发**）。
8. **并发写者协调**：按 spec §8 执行 —— 默认静态分区（一个写者一个目录）+ 两把显式锁
   （`work/tmp/locks/game.lock` 游戏内实验串行；`ledger.lock` 共享账本由主导者独占写）。

### 待定

- benchmark 协议与性能目标：暂缓（测法已定，见主设计 §7.3）。

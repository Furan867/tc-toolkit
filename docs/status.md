<!-- 整体重写，禁止追加。≤80 行。
     一次性结论 → notes/；被复用过的结论 → facts/。具体事实不在这里复制。 -->
# 状态

在追：**Simplex → LLVM 编译器的 M5.5 分层重构**，已进入收口（2026-10-24）。

目标：把 `compile.dll` 换成自研实现（Simplex → LLVM IR → 装配成游戏可 JIT 的 blob）。
正确性判据：同一份 `.si`，我们的产物与参考产物**逐 tick 状态等价**。
验收边界（2026-10-18 负责人确认）：沙盒模式只看 RV32IM 电路。

- 接口权威 = `docs/specs/2026-10-15-simplexc-interfaces.md`（冻结版 1.4）；主设计 = `…-simplex-llvm-compiler-design.md`；
  主频层 = `…-freq-measurement-design.md`。代码在 `reversing/simplexc/`；LLVM 固定 **23.1.3**。
- 重构方案、HIR 规格、内建编号、错误三轴、S2c 切割底数：**`docs/notes/2026-10-24-refactor-plan.md`（唯一入口）**。

## 已完成（有判据）

- M0 观测设施 · M1 spike · M2 差分宿主 · 装配器 · M4.5 游戏内 40.375 vs 5.364 MHz。
- **判据链**：`blob_baseline --verify`（98 份全量 8.6s，比掩掉 stub/slot 槽位的 `norm_sha256`）已进
  `check.py`（6 步，46s 全绿）。核心判据是 `python simplexc/tools/blob_baseline.py --verify simplexc/tools/baseline`。
- **`sema/` 完成**：类型系统、HIR 六条不变量（每条一个不满足的样例，报对编号）、生成器、内建 3..11、
  前缀函数标记、方法糖、聚合常量、实例闭包。**语料 80/98，基线里出产物的 75 份全过**；24 个最小用例
  `analyze_selftest` 全绿；零 LLVM 依赖、不反向 include `frontend/`。
- **`codegen` 结构改造完成**（`a791b83`）：`FnState`/`save_state`/`restore_state` 归零，每函数一个 `FnCtx`，
  产物 98/98 逐字节不变。
- **`codegen` 吃 HIR**（`49568d8` 已切产品路径）：双跑对照 `work/tmp/s2c/dual.py`
  —— 同一份 `.si` 走「旧 AST 路径」与「sema→HIR→新 codegen」，比 blob 逐字节 + IR 逐函数 ——
  **same 75/75**（跳过的 23 份是旧路径本来就编不过的 `hex`/`bin`/`str` 那批）。
  **切换后 `blob_baseline` 仍 98/0**：这是整个重构的最终验收，双跑等价 + 切换后产物不变，两者都过了。

## 下一步（按串行依赖）

1. **S3 错误三轴**：`CompileError{resp,stage,span,detail}` 过 C 边界，消除 P1..P6 六条静默降级
   （底数：252 抛点 / 50 捕获点 / 23 处必须全覆盖，工单在 `work/tmp/errors-cut/`）。
   **不与 1–3 混做**：分层重构的判据是「产物不变」，错误三轴必然改失败输出，混做无法归因。
5. **S4**：codegen 侧最小用例；之后重跑 M5 判据、重记数字。

## 待定 / 未解

- M5 的语料数字（98 份：75 可装配 / 30 全等 / 14 diff / 23 cli_fail）**全部标为不确定**，重构完成后重跑重记。
- 刻意不做（等重构收口）：`hex`/`bin`/`str` 前缀格式化、`ui_set_instruction()` 重载 —— 那 16 份语料基线
  本来就不出产物，现在补等于新增能力，会污染「产物逐字节相同」这条判据。
- 未验语义：`U-L4` 偏移/padding · `$` 写回时机 · `U-L5` 显式宽度语法 · `Str == Str` 编译期折叠 · `@Type` 值形实参。
- 已知未修差异：顶层非声明语句被整体丢掉 · 参考对 `return a) + 1` 静默吃掉后面的一切 · 空数组的指针值未对照。
- `entry`（`Result[2]`）用途仍只知「相对机器码起点的偏移」；参考产物的热环没定位到（要 RIP 采样）。

## 环境

- 只有一条硬纪律要记：**`build/` 是共享的**，两个写者同时跑 `build.py`/`check.py` 会互相覆盖产物；
  多写者时按目录切分，且同一时刻只让一个写者跑判据入口。

<!-- 整体重写，禁止追加。≤80 行。
     一次性结论 → notes/；被复用过的结论 → facts/。具体事实不在这里复制。 -->
# 状态

在追：**Simplex → LLVM 编译器的 M5.5 分层重构**（2026-10-24 起为最高优先）。

目标：把 `compile.dll` 换成自研实现（Simplex → LLVM IR → 装配成游戏可 JIT 的 blob）。
正确性判据：同一份 `.si`，我们的产物与参考产物**逐 tick 状态等价**。
验收边界（2026-10-18 负责人确认）：沙盒模式只看 RV32IM 电路。

- 接口权威 = `docs/specs/2026-10-15-simplexc-interfaces.md`（冻结版 1.4）；主设计 = `…-simplex-llvm-compiler-design.md`；
  主频层 = `…-freq-measurement-design.md`。代码在 `reversing/simplexc/`；LLVM 固定 **23.1.3**。
- 重构方案、HIR 规格、错误三轴、S3 工单底数：**`docs/notes/2026-10-24-refactor-plan.md`（唯一入口，本文件不复述）**。

## 已完成（确定为真）

- M0 观测设施 · M1 spike · M2 差分宿主 · 装配器（桩/槽两趟定址、`.stubs` sidecar、返回前自校验）。
- M4.5：C++17 前端 + lowering + CLI + 壳接线；游戏内 40.375 vs 5.364 MHz（7.53×）、无 fallback。
- **M5.5 的判据链**（这是重构能继续的前提）：
  - 产物基线回归 `python simplexc/tools/blob_baseline.py --verify simplexc/tools/baseline` ——
    98 份全量 **8.6s**，比掩掉 stub/slot 槽位的 `norm_sha256`（raw sha 跨 boot 会变，见 `facts/build-assemble.md`）；
    已进 `check.py`（6 步，50s 全绿）。
  - 分层地基：`sema/` 拿到类型系统 + 常量折叠（269 行逐行搬，无 LLVM 依赖）。
  - HIR 已冻结并落码：节点集、六条不变量的自检（每条各有一个不满足的样例并报对编号）、可 diff 的 dump + golden。
  - **sema 生成器约六成**：程序根、表达式、类型解析、VarDecl/Assign/Return/Block/While/Break、隐式转换显式化成 `Cast`；
    `analyze_selftest` 5 个最小用例 + golden。`sema/` 零 LLVM 依赖、不反向 include `frontend/`。

## 下一步（已决定）

1. **S2b-2**（在跑）：`If`（含 `in_scope` 剪枝，裸 `if` 里也有）/`Switch`/数组字面量/struct 与 enum 布局/`Str` 与模板/前缀全局。
2. **S2b-3**（最高风险）：把「发函数体时又发现新实例」的按需递归改成 sema 先算完实例闭包；
   实例键规范化、`cap_index` 编号顺序必须与现状 `$c<n>` 一致。伪代码规格与语料形状统计已派出。
3. **S2c**：codegen 一次性改吃 HIR，删 `static_type`/`unify`/`arith_ty`/`const_value` 等推理，
   产物符号名按现状规则派生。切割底数在 `work/tmp/codegen-cut/`（802 行 AST 引用 + 44 个推理调用点）。
4. **S2d**：拆 `V`/`FnState`，消除 save/restore（设计 §3.3 的硬要求）。
5. **S3**：错误三轴过 C 边界 + 消除 P1..P6 六条静默降级（底数：252 抛点 / 50 捕获点 / 23 处必须全覆盖）。
6. **S4**：§7.6 最小用例体系；之后重跑 M5 判据、重记数字。

依赖是串行的 `S2b-2 → S2b-3 → S2c → S2d`（同一目录单写者），`S3` 的只读测绘已完成、可随时插。

## 工作纪律（本轮定）

- 中间提交**不必** `check.py` 全绿；但「产物逐字节相同」这条判据要跑（8.6s，是唯一的定位手段）。
- `S2c` 切换期允许基线变红，按语料逐个收敛，不要求一次性全绿。
- 允许边写边删，双份实现只保留到 sema 能覆盖大部分形态为止。

## 待定 / 未解

- M5 的语料数字（98 份：75 可装配 / 30 全等 / 14 diff / 23 cli_fail）**全部标为不确定**，重构完成后重跑重记。
- 未验语义：`U-L4` 偏移/padding · `$` 写回时机 · `U-L5` 显式宽度语法 · `Str == Str` 编译期折叠 · `@Type` 值形实参。
- 窄位宽/移位语义已收口（对 98 份无收益，不再投入）。
- 已知未修差异：顶层非声明语句被整体丢掉 · 参考对 `return a) + 1` 静默吃掉后面的一切 · 空数组的指针值未对照。
- `entry`（`Result[2]`）用途仍只知「相对机器码起点的偏移」；参考产物的热环没定位到（要 RIP 采样）。

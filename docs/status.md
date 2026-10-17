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

**设计与接口**：
- **接口权威 = `docs/specs/2026-10-15-simplexc-interfaces.md`（冻结版 1.1）**；主设计
  `docs/specs/2026-10-15-simplex-llvm-compiler-design.md` 只定分区与纪律（冲突以接口冻结版为准）。
  1.1 增量：产物 `AsmResult::stubs` + `<blob>.stubs` sidecar（宿主靠它认出 extern 跳板并接桩）；
  1.0 对齐：`ExternResolver&`、`AsmStatus` 补 `BadObject`/`TooLarge`、桩长 12 字节、
  `impl` 词表 = `reference | ours | fallback | ours_failed`。
- 主频层 `docs/specs/2026-10-15-freq-measurement-design.md`（5 项决策已确认；实现未开始）。
- 代码落库 `reversing/simplexc/`；LLVM 固定 **23.1.3**（`bin/clang.exe` 已解出，sha256 记在 `facts.md`）。

**已完成**：
- **M0 观测设施**：98 份语料 + 含 `entry` 的 manifest + 54×98 覆盖率矩阵。
- **M1 spike**：装配链跑通、游戏内 JIT 执行过（g++ 来源的空程序）。
- **M1.5 收益探针（宿主内）**：clang -O2 的 006 等价物对参考产物 **10/10 tick 全等、白名单为空**；
  收益来自「热状态进寄存器」而不是「直译成 C 交给 clang」（直译慢 ~25%，热计数器变体快 ~2.95×）——
  **游戏内那一枪还没开**（真实收益量级要它定）。
- **M2 差分宿主**：参考产物自比对通过；`<blob>.stubs` sidecar 让我们的产物也能跑同一驱动。
- 基础设施：`simplexc/tools/build.py` 一键构建、`check.py` 判据入口、`verify_game_contract.py`（§10 重验）、
  壳的失败路径与熔断（ours 失败记账 / §8 分类 / ours 超时 / VEH / per-call `impl` 行）。

### 下一步（已决定，按顺序）

1. **游戏内验证（要 `game.lock`）**：把 `ours` 的产物从空程序换成 006 的等价物（它驱动完整仿真协议，
   这正是 pitfalls 里「空程序会让下一次编译请求 abort」的正确解法）→ 用 `reference` 起游戏再热切 →
   闭合「LLVM 来源的 blob 真正执行过一次」，并顺便读游戏内主频。⚠️ 需要负责人配合（起游戏、打开对应电路）。
2. **假状态保真度**（M2 遗留）：让 001 那类「解引用游戏给的指针/结构」的电路也能在宿主里跑（现在崩在写地址 0）
   —— 这是 M3 全语料差分的前置，要改 `host.py`。
3. **M3 语义档案**：接口 §9 的 U-L1…U-L14 **必须实验后才写死**（`load`/`store` 的地址单位是地基）；
   每条 = 最小 `.si` + 驱动方式 + 参考产物下的可观测量，机器可执行（`simplexc/dossier/`）。
4. **定 MVP 必须支持的构造集合**：以 `coverage.tsv` 的 98/98 构造为下界，加上 stdlib 前缀里那 6 个，交负责人确认。
5. **前端 + Lowering**（M3 主体）：按接口冻结版 1.1 实现。**已有一条硬指示**：游戏内存是 `volatile`，
   lowering 必须让私有热状态有「有序但不与游戏内存别名」的语义（照抄全 volatile 会把收益吃光，见 `facts.md`）。
6. **主频/bench 层重写**：`tools/freq/` 包 + `hook.log` 迁移 + per-call `impl` 行消费。
   与壳抢同一份 `tools/compile_hook.c` → 串行，锁 `work/tmp/locks/compile-hook.lock`。

### 待定 / 未解

- `entry`（`Result[2]`）的用途仍只知「相对机器码起点的偏移」；参考产物 entry 自己 push 全部寄存器。
- 产物里 **75300 处非 image 空间 movabs**（大整数常量、`0x232a…` 游戏地址）跨 boot 相同的机制没查。
- `campaign/` 的 92 份没编译过（可能是片段，编译失败会 abort 整个进程）。
- 字面量的 `'…'`（单引号）与模板插值的语义未定（`docs/notes/2026-10-16-m0-coverage.md` 有单列）。
- 006 太小：游戏内 A/B 需要「输入真的在变」的电路；参考产物的热环也没定位到（要动态采样，不猜反汇编）。
- 主设计 §11 的 U1–U4、U7：未动。U5（内建清单完备性）已有权威来源（DLL 的 `.rdata` 字面量池），待提取成表。

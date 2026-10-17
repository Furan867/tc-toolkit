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
  1.1 增量：`AsmResult::stubs` + `<blob>.stubs` sidecar；桩长 12 字节；`impl` 词表
  `reference | ours | fallback | ours_failed`。
- 主频层 `docs/specs/2026-10-15-freq-measurement-design.md`（决策已定；实现未开始）。
- 代码落库 `reversing/simplexc/`；LLVM 固定 **23.1.3**（`bin/clang.exe`，sha256 在 `facts.md`）。
- **当前没有前端、没有 IR**：`frontend/`、`lower/` 只有 README。M1.5 的产物是**手写 C**，
  优化全由 `clang -O2` 做（`default<O2>`）；我们只负责写法选择与装配。

**已完成**：
- **M0 观测设施**：98 份语料 + 含 `entry` 的 manifest + 54×98 覆盖率矩阵。
- **M1 spike**：装配链跑通（g++ 来源的空程序，728 B）。
- **M1 缺口已闭合**：clang 来源的产物在游戏进程里跑到过协议主函数（`manifest.tsv` seq 007，
  `impl=ours`，标记文件两行）—— 见 `facts.md`。同一次触发随后崩，**根因是用过期地址部署**，非逻辑错。
- **M1.5 收益探针（宿主内）**：006 等价物对参考产物 10/10 tick 全等、白名单为空；
  忠实直译 1.74 ns/cycle（**慢 28%**），热状态进寄存器 0.47 ns/cycle（**快 2.9×**）→
  收益来自 lowering 的别名/定序策略，不来自 clang 的 pass（硬指示，见 `facts.md`）。
- **M2 差分宿主**：`<blob>.stubs` sidecar 让我们的产物也能跑同一驱动；`selftest.py --entry`。
- 装配器两处修复：`.bss` 无原始数据节（产物可复现）、桩/槽分两趟定址（`__imp_` 被桩块挤歪），
  且返回前自校验 sidecar ↔ 产物字节。
- 工具链：`build.py`（一键构建）、`check.py`（4 步判据入口）、`verify_game_contract.py`（§10 重验）、
  `gen_addrs.py` + `equiv_build.py`（**现取地址** → clang -O2 → 装配 → 双跑宿主 → 逐 tick 比对）。
- 壳：失败路径与熔断（ours 失败记账 / §8 分类 / 超时 / VEH / per-call `impl`）。

### 下一步（已决定，按顺序）

1. **拿到一个「真有组件」的靶子**（阻塞 M1.5 游戏内 A/B）。现状：`work/hook/hook_src_008.si` 是
   元件工坊上下文但 `get_component_count()=0`、`mode_refresh()` 空（每 tick 只 `cycle += 1`），
   测不出收益；040（与门，4 组件）已被新会话覆盖。→ 负责人在元件工坊里对**计数器元件**触发一次编译，
   捕获里必须出现 `com_counter` 一类组件调用与 `get_component_count() > 0`。
2. **等价物骨架模板化**（进行中，`skeleton` 子任务）：把协议骨架与组件部分拆开，换电路只写组件那几十行。
   判据：006 与 008 两份不同上下文都由同一骨架 + 各自组件部分编出，逐 tick 全等。
3. **游戏内 A/B 主频**（要 `game.lock`，依赖 1）：部署纪律 = 用**最新** `hook_src_NNN.si` 现取地址 →
   宿主逐 tick 全等 → 部署 → 切 `ours` → 持续跑采样 → 立刻切回 `reference`。
   单与门/计数器的比值会被协议固定开销淹没，**优先选每 tick 有实质运算**的组件。

### 待定 / 未解

- 手写 C 等价物能覆盖的电路复杂度有限（用户判断）；要不要跳过 M1.5 的更多手写、直接推 M3 前端，**未定**。
- `entry`（`Result[2]`）的用途仍只知「相对机器码起点的偏移」。
- 产物里 75300 处非 image 空间 movabs（大整数常量、`0x232a…` 游戏地址）跨 boot 相同的机制没查。
- `campaign/` 的 92 份没编译过（可能是片段，编译失败会 abort 整个进程）。
- 字面量 `'…'`（单引号）与模板插值的语义未定（见 `docs/notes/2026-10-16-m0-coverage.md`）。
- 参考产物的热环没定位到（要 RIP 采样，不猜反汇编）。
- M3 语义档案（§9 的 U-L1…U-L14）、MVP 构造集合、前端 + lowering、主频/bench 层重写：未开始。

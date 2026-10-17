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
- 主设计 `docs/specs/2026-10-15-simplex-llvm-compiler-design.md`（产物契约 T1–T5 闭合）。
- **接口冻结版 1**：`docs/specs/2026-10-15-simplexc-interfaces.md`（主设计 §8 的 7 项 + §9 未定项 U-L1…U-L14）。
  已批准取舍：runtime 目标码随 blob 装配／blob 内部禁止绝对重定位／corpus 的 `.bin` 不进 git。
- 主频层 `docs/specs/2026-10-15-freq-measurement-design.md`（5 项决策已确认；实现未开始）。
- 代码落库 `reversing/simplexc/`；LLVM 固定 **23.1.3（官方 win64 包）**，链接方式见 `facts.md` §构建与 LLVM。

**M0 观测设施：已完成**（2026-10-16）
- 语料 `corpus/src/` 98 份 + `corpus/ref/` 产物 + `manifest.tsv`（13 列，含 `entry`）+ `stdlib_prefix.si` + `campaign/` 92 份；
  判据 `recompile_corpus.py --check` rc=0；归一化 DLL 基址后逐字节相同 98/98。
- 覆盖率矩阵 `corpus/coverage.tsv`（54 构造 × 98 文件）；**98/98 的构造 37 个**；
  `type alias`/`@Any`/`$ 参数`/`asm`/`extern`/`big_int_*` 在语料 0 次但全在 stdlib 前缀里。

**M1 spike：已完成**（2026-10-17）
- 装配链跑通（进程内 + **游戏内**）：LLVM 模块或 g++ 目标文件 → 装配器（+ runtime + shim）→ blob → 游戏 JIT 执行。
- 进游戏实测：`manifest.tsv` seq 006 = 我们的产物、`impl=ours`、`work/tmp/blob_ran.log` 出现 blob 自己写的标记
  → 我们的代码真在游戏进程里跑到了；游戏没崩。详情 `docs/notes/2026-10-17-m1-spike-游戏内.md`。
- ⚠️ 空程序只能证链路：占着真实电路会让游戏在下一次编译请求时 abort（见 `facts.md`）。
- 仍未做：LLVM 模块接进壳（壳现在读预装配 blob）、VEH 熔断。

**M2 差分宿主：已完成**（2026-10-17）—— 参考产物自比对通过，宿主可靠性得证。
- 代码 `simplexc/tools/host/{simspec.py,host.py,selftest.py}`；设计取证 `docs/notes/2026-10-17-m2-host-design.md`。
- 做法：加载真 `compile_real.dll`（产物会直接调它的运行时函数、读写它的 `.bss`）→ 把产物里 baked 基址
  重定位到实际加载基址 → 按源码地址表 `VirtualAlloc` 假游戏内存 → detour 危险导入 → `jmp` 语义的
  trampoline 起线程 → 递增 `sim_target_cycle`/命令号驱动，逐 tick 出快照（页 sha256 + extern 序列）。
- 判据与结果（`selftest.py`，两个独立宿主进程）：**005/006/120 通过**（逐 tick 全等 + 仿真真在工作）；
  020/026 逐 tick 全等但电路太小（无工作页，不算过）；001（地图电路）崩在「写地址 0」= 假状态保真度不够。
- 未解决：假游戏状态只做到「零填 + 数组头」——凡是要解引用游戏给的指针/结构的电路（001 那类）跑不了。

### 下一步（已决定）

1. **假状态保真度**：让 001 那类电路也能在宿主里跑（现在写地址 0）——这是 M3 全语料差分的前置。
2. **M3 语义档案**：接口文档 §9 的 U-L1…U-L14 **必须实验后才写死**（`load`/`store` 的地址单位是地基）；
   每条 = 最小 `.si` + 驱动方式 + 参考产物下的可观测量，机器可执行、由脚本采集（主设计 §7.4）。
3. **定 MVP 必须支持的构造集合**：以 `coverage.tsv` 的 98/98 构造为下界，加上 stdlib 前缀里那 6 个，交负责人确认。
4. **前端 + Lowering**（M3 主体）：按接口冻结版 1 实现，产物先接离线 CLI，再接壳。
5. **M1.5 收益探针**：最小电路端到端主频 A/B（先量量级）。
6. **判据入口**（§7.5）：全语料编译 → 逐 tick 差分 → 覆盖率矩阵 + 回退率；游戏内部分标 slow/manual。
7. **主频/bench 层重写**：`tools/freq/` 包 + `hook.log` 迁移 + per-call `impl` 行。与 spike 抢同一份
   `tools/compile_hook.c` → **串行（spike 先、freq 后）**，锁文件 `work/tmp/locks/compile-hook.lock`。
8. **Steam 更新后的重验脚本**（§10 的 5 条断言，按符号名定位、不硬编码地址）。

### 待定 / 未解

- `entry`（`Result[2]`）的用途仍只知「相对机器码起点的偏移」；参考产物 entry 自己 push 全部寄存器。
- 产物里 **75300 处非 image 空间 movabs**（大整数常量、`0x232a…` 游戏地址）跨 boot 相同的机制没查。
- `campaign/` 的 92 份没编译过（可能是片段，编译失败会 abort 整个进程）。
- 字面量的 `'…'`（单引号）与模板插值的语义未定（`docs/notes/2026-10-16-m0-coverage.md` 有单列）。
- benchmark 协议与性能目标：暂缓（测法已定，主设计 §7.3）。
- 主设计 §11 的 U1–U4、U7：未动。U5（内建清单完备性）已有权威来源（DLL 的 `.rdata` 字面量池），待提取成表。

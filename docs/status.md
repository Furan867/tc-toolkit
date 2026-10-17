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
  2026-10-16 批准的取舍：runtime 目标码随 blob 装配（自包含）／blob 内部禁止绝对重定位／corpus 的 `.bin` 不进 git。
- 主频层 `docs/specs/2026-10-15-freq-measurement-design.md`（5 项决策已确认；实现未开始）。
- 代码落库 `reversing/simplexc/`；LLVM 固定 **23.1.3（官方 win64 包）**，链接方式见 `facts.md` §构建与 LLVM。

**M0 观测设施：已完成**（2026-10-16，两个子 agent + 主程独立复算）
- 语料：`corpus/src/` 98 份唯一源码、`corpus/ref/` 产物（不进 git）、`corpus/manifest.tsv` 13 列（含 `entry`）、
  `corpus/stdlib_prefix.si`、`corpus/campaign/` 92 份。判据：`recompile_corpus.py --check` 退出码 0；
  `entry` 有效 98/98；归一化 DLL 基址后逐字节相同 98/98（未分类差异 0）。
- **覆盖率矩阵**：`corpus/coverage.tsv`（54 构造 × 98 文件）、`tools/tag_corpus.py`（掩码扫描 + `--check` + `--evidence`）。
  **98/98 的构造有 37 个**；`type alias`/`@Any`/`$ 参数`/`asm`/`extern`/`big_int_*` 在 98 份里 0 次出现，
  但**全在 stdlib 前缀里**（不能当「不必支持」）。笔记 `docs/notes/2026-10-16-m0-coverage.md`。
- 工具：`simplexc/tools/{recompile_corpus.py, classify_abs_addrs.py, tag_corpus.py}`、`tools/extract_nim_strings.py`、
  `tools/fetch_llvm.py`。

**M0/M1 前置项（§11 的 U10/U11/U12）已答，结论进 `docs/facts.md`**：
- U12：`#` 只有 `#if in_scope("…")` 与 `const #NAME=` 两类。
- U11：生成源码把游戏内存地址硬编码成编译期常量（`0x232a…` 段，实测全新进程可直接 `VirtualAlloc` 到原地址
  → 差分宿主不必重写地址）；`Arr` 头布局必须与 Nim seq 同构。
- U10：extern 调用走 Win64 ABI，且调用目标 = `compile_real.dll` 自己的导入 thunk → 参考产物绑定 DLL 加载基址，
  **产物跨 boot 不能逐字节相等**。

**M1 spike：装配链路已跑通（进程内自测），未进游戏**（2026-10-17）
- `LLVM-C.dll` 已取到并**用 MinGW 链上**（`gendef` + `dlltool` 造导入库）；`LLVMRunPasses(default<O2>)` +
  `LLVMTargetMachineEmitToMemoryBuffer(ObjectFile)` 拿到 COFF 字节（`simplexc/shell/llvm_link_probe.cc`）。
- **装配器可用**：`simplexc/assemble/`（COFF 解析 → 节归并/丢弃 → REL32 打补丁 → extern 桩与 `__imp_` 指针槽
  → 8 字节头 + bit62 + entry）。`simplexc/shell/entry_shim.S`（入口 shim + `___chkstk_ms`）、
  `simplexc/runtime/rt_min.cc`（`thread_exit`）、`simplexc/cli/tcblob.cc`（CLI + `--run` 自测）。
- 判据（已跑）：`tcblob --obj rt_min.o --obj entry_shim.o --obj prog_empty.o --entry __tc_entry --run`
  → `RUN ok：blob 在 … 上跑完，线程退出码 0`；产物 128 字节，头 = `0x80 | bit62`，entry=0x10。
**M1 spike：已完成**（2026-10-17）—— 自研 blob 在游戏内被 JIT 执行、游戏没崩。
- 装配链在进程内与**游戏内**都跑通：LLVM 模块（`tc_main`）或 g++ 目标文件 → 装配器（+ runtime + shim）→ blob
  → 游戏 `jit` 分配可执行内存、从 blob+8 拷、`jmp entry`。
- **进游戏实测**（详情 `docs/notes/2026-10-17-m1-spike-游戏内.md`）：`manifest.tsv` seq 006 = 我们的产物
  （728 字节 / `3b378baa…`）、`impl=ours`、`hook.log` 有 `impl=ours result=ok`、
  `work/tmp/blob_ran.log` 出现 `blob ran, tag=23063`（blob 里 `tc_spike_mark` 写的，证明确实在游戏进程里跑到了）、
  进程存活且窗口有应答、Windows Application 日志无崩溃事件（bit62 的间接证据：否则游戏会拿 Nim 分配器 free 我们的指针）。
- ⚠️ 姿势：**先用 `reference` 启动游戏，再热切 `compiler = ours`**，且只在小电路上触发一次编译；
  带着 `ours` 启动会因地图电路拿到空程序而看着像起不来（实测过）。
- ⚠️ **空程序只能用来证链路**：负责人随后手动编辑触发下一次编译时，**游戏 abort 了**（死在 `compile()` 之前，
  无 WER/无 dump；机制与特征见 `facts.md`）。后续游戏内验证必须等前端能产出「能驱动完整仿真协议」的程序。
- 仍未做：LLVM 模块接进壳（现在壳用的是预装配 blob）、VEH 熔断、游戏内差分（M2）。

### 下一步（已决定）

1. **M2 观测设施（下一步）**：离线差分宿主（假游戏内存 + extern 桩 + 逐 tick 驱动），
   先用「参考产物 vs 参考产物」自比对证明宿主可靠 —— 它必须先于前端存在，否则前端写完也没有尺子。
2. **定 MVP 必须支持的构造集合**：以 `corpus/coverage.tsv` 的 98/98 构造为下界，加上 stdlib 前缀里出现的那 6 个
   （`type alias`/`@Any`/`$ 参数`/`asm`/`extern`/`big_int_*`），交负责人确认。
3. **M3 语义档案**：接口文档 §9 的 U-L1…U-L14 **必须实验后才写死**（`load`/`store` 的地址单位是地基）。
4. **M2 差分宿主**：先用「参考产物 vs 参考产物」自比对证明宿主可靠，再接我们的产物。
5. **M1.5 收益探针**：最小电路端到端主频 A/B（先量量级）。
6. **判据入口**（§7.5）：全语料编译 → 逐 tick 差分 → 覆盖率矩阵 + 回退率；游戏内部分标 slow/manual。
7. **主频/bench 层重写**：`tools/freq/` 包 + `hook.log` 迁移 + per-call `impl` 行。与 spike 抢同一份
   `tools/compile_hook.c` → **串行（spike 先、freq 后）**，锁文件 `work/tmp/locks/compile-hook.lock`。
8. **Steam 更新后的重验脚本**（§10 的 5 条断言，按符号名定位、不硬编码地址）。

### 待定 / 未解

- `entry`（`Result[2]`）的**用途**没验证：只知它是相对机器码起点的偏移，与顶层块 / shim 落点的关系待 spike 定。
- 产物里另有 **75300 处非 image 空间 `movabs`**（大整数常量、`0x232a…` 游戏地址）跨 boot 完全相同，
  机制没查。
- `campaign/` 的 92 份没编译过（可能是片段，编译失败会 abort 整个进程）；是不是完整程序未知。
- 字面量的 `'…'`（单引号）与模板插值的语义未定（`docs/notes/2026-10-16-m0-coverage.md` 有单列）。
- benchmark 协议与性能目标：暂缓（测法已定，主设计 §7.3）。
- 主设计 §11 的 U1–U4、U7：未动。U5（内建清单完备性）已有权威来源（DLL 的 `.rdata` 字面量池），待提取成表。

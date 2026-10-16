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

**M1 spike：第一个验收点已过**（2026-10-16）
- `LLVM-C.dll` 已取到并**用 MinGW 链上**（`gendef` + `dlltool` 造导入库）；`LLVMRunPasses(default<O2>)` +
  `LLVMTargetMachineEmitToMemoryBuffer(ObjectFile)` 拿到 COFF 字节（`simplexc/shell/llvm_link_probe.cpp`，
  最后验证输出 `RESULT: OK`）。
- 装配器的**输入形状已实测**（节、三类 REL32 目标、`.pdata` 的 `ADDR32NB`、未定义符号形态）→ 见 `facts.md`。

### 下一步（已决定）

1. **M1 继续**：写 `simplexc/assemble`（COFF 解析 → 合并节 → REL32 应用 → 外部符号 stub pool → 8 字节头 bit62
   → entry 偏移 + 生命周期）+ 手写 entry shim（保存寄存器 + 对齐栈）+ 壳里加 `compiler` 三模式开关
   → 游戏内跑通空程序（不崩、bit62 生效）。`tools/compile_hook.c` 这段时间只归 spike 用。
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

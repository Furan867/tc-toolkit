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
- 主设计 `docs/specs/2026-10-15-simplex-llvm-compiler-design.md`（产物契约 T1–T5 闭合；LLVM 链接 = `LLVM-C.dll` + C API + w64devkit g++）。
- **接口冻结版 1**：`docs/specs/2026-10-15-simplexc-interfaces.md`（主设计 §8 的 7 项 + §9 未定项 U-L1…U-L14）。
  2026-10-16 批准的取舍：runtime 目标码随 blob 装配（自包含）／blob 内部禁止绝对重定位／corpus 的 `.bin` 不进 git。
- 主频层 `docs/specs/2026-10-15-freq-measurement-design.md`（5 项决策已确认；实现未开始）。
- 代码落库 `reversing/simplexc/`（frontend / lower / runtime / assemble / shell / cli / tools / corpus / dossier）。

**M0 第一件事已完成**（2026-10-16，子 agent 执行 + 主程独立复算）：
- `simplexc/corpus/src/` 98 份唯一源码（180 份捕获按字节去重）、`corpus/ref/` 98 份产物（不进 git）、
  `corpus/manifest.tsv` 13 列（含 `entry`）、`corpus/stdlib_prefix.si`（27656 B）、`corpus/campaign/` 92 份关卡测试。
- 判据通过：`python simplexc/tools/recompile_corpus.py --check` 退出码 0；`entry` 有效 98/98；
  `out_len`/`out_sha256` 与文件一致 98/98；归一化 DLL 基址后逐字节相同 98/98（差值恒 `0x380000`，未分类差异 0）。
- 工具：`simplexc/tools/{recompile_corpus.py, classify_abs_addrs.py}`、`tools/extract_nim_strings.py`。
- 笔记：`docs/notes/2026-10-16-m0-corpus.md`。

**M0/M1 前置项（§11 的 U10/U11/U12）已答，结论进 `docs/facts.md`**：
- U12：`#` 只有 `#if in_scope("…")` 与 `const #NAME=` 两类。
- U11：生成源码把游戏内存地址硬编码成编译期常量（`0x232a…` 段，实测全新进程可直接 `VirtualAlloc` 到原地址
  → 差分宿主不必重写地址）；`Arr` 头布局必须与 Nim seq 同构。
- U10：extern 调用走 Win64 ABI，且调用目标 = `compile_real.dll` 自己的导入 thunk
  （2026-10-16 更正「直接调 kernel32」的旧判读）→ 参考产物绑定 DLL 加载基址，**产物跨 boot 不能逐字节相等**。

### 下一步（已决定）

1. **M0 收尾**：按「用到的构造」给 98 份语料打标签 → 特性 × 文件覆盖率矩阵；
   沙盒模式用到的构造集合 = MVP 验收下界，其余标「可回退」。（子 agent 可做，静态分区 `simplexc/corpus` 只读 + 新脚本）
2. **M1 spike**（等负责人批准下载 LLVM）：取 `LLVM-C.dll`（首选官方 win64 包 263.5 MB，
   只解 `bin/LLVM-C.dll` + `include/llvm-c/**` 到 `reversing/build/llvm/`，用 `gendef`+`dlltool` 造导入库）
   → `LLVMRunPasses` + `LLVMTargetMachineEmitToMemoryBuffer` 拿到 COFF 字节 → 装配器 → 手写 entry shim
   → 游戏内跑通空程序（bit62 置 1 不被 free、不崩游戏）。
3. **M1.5 收益探针**：最小电路端到端主频 A/B（先量量级）。
4. **M3 语义档案**：接口文档 §9 的 U-L1…U-L14 **必须实验后才写死**（`load`/`store` 的地址单位是地基）。
5. **M2 差分宿主**：先用「参考产物 vs 参考产物」自比对证明宿主可靠，再接我们的产物。
6. **判据入口**（§7.5）：全语料编译 → 逐 tick 差分 → 覆盖率矩阵 + 回退率；游戏内部分标 slow/manual。
7. **主频/bench 层重写**：`tools/freq/` 包 + `hook.log` 迁移 + per-call `impl` 行。与 spike 抢同一份
   `tools/compile_hook.c` → **串行（spike 先、freq 后）**，锁文件 `work/tmp/locks/compile-hook.lock`。
8. **Steam 更新后的重验脚本**（§10 的 5 条断言，按符号名定位、不硬编码地址）。

### 待定 / 未解

- **M1 的 LLVM 下载等批准**（方案对比见 2026-10-16 的报告：官方包 vs MSYS2 pacman）。
- 产物里另有 **75300 处非 image 空间 `movabs`**（`0x2540BE400` 这类大整数常量、`0x232a…` 这类游戏地址）
  跨 boot 也完全相同 —— 机制没查；若它们其实是地址，说明还有第二处「碰巧稳定」的分配。
- `campaign/` 的 92 份没编译过（可能是片段，编译失败会 abort 整个进程）；它们是不是完整程序未知。
- `entry`（`Result[2]`）的**用途**没验证：只知它是相对机器码起点的偏移，与顶层块/ shim 落点的关系待 spike 定。
- benchmark 协议与性能目标：暂缓（测法已定，主设计 §7.3）。
- 主设计 §11 的 U1–U4、U7：未动。U5（内建清单完备性）已有权威来源（DLL 的 `.rdata` 字面量池），待提取成表。

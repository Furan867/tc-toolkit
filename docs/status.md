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

**设计定稿**：
- 主设计 `docs/specs/2026-10-15-simplex-llvm-compiler-design.md`（产物契约由 T1–T5 闭合；§7.5 判据入口 /
  §8 工作流 / §10 重验脚本；LLVM 链接 = `LLVM-C.dll` + C API + w64devkit g++）。
- **接口冻结版 1：`docs/specs/2026-10-15-simplexc-interfaces.md`**（主设计 §8 的 7 项）。
  2026-10-16 批准的取舍：runtime 目标码随 blob 装配（自包含）／blob 内部禁止绝对重定位／corpus 的 `.bin` 不进 git。
- 主频层 `docs/specs/2026-10-15-freq-measurement-design.md`（5 项决策已确认；实现未开始）。
- 代码落库 `reversing/simplexc/`（frontend / lower / runtime / assemble / shell / cli / tools / corpus / dossier）。

**M0/M1 前置项（§11 的 U10/U11/U12）已答，结论进 `docs/facts.md`**：
- U12：`#` 只有 `#if in_scope("…")`（4 种实参形态）与 `const #NAME=` 两类 → 预处理器范围就这两块。
- U11：生成源码把游戏内存地址硬编码成编译期常量；`get_memory`/`set_ram_value` 等是生成源码里的**空桩** →
  离线宿主必须按绝对地址造假内存；`Arr` 头布局必须与 Nim seq 同构。
- U10：extern 调用走 Win64 ABI（`sub rsp,imm16` 影子空间），**且调用目标 = `compile_real.dll` 自己的导入 thunk**
  （2026-10-16 更正：原判读为"直接调 kernel32"）→ 参考产物绑定 DLL 加载基址，跨 boot 不能逐字节相等。

### 下一步（已决定）

1. **M0 收尾**：98 份唯一源码 + 含 entry 的 manifest（子 agent 在跑）→ 构造标签 → 覆盖率矩阵
   （沙盒模式用到的构造 = MVP 验收下界）。判据：差异全部可解释且归一化到 DLL 镜像偏移后逐字节相同。
2. **M1 spike**：取 `LLVM-C.dll`（先试 MSYS2 mingw-w64 LLVM 包，备选官方 win64 包 + `gendef`/`dlltool`）
   → `LLVMRunPasses` + `LLVMTargetMachineEmitToMemoryBuffer` 拿 COFF 字节 → 装配器 → 手写 entry shim
   → 游戏内跑通空程序（bit62 置 1 不被 free、不崩游戏）。**先钉死这台的 LLVM 版本与 sha256 再写代码。**
3. **M1.5 收益探针**：最小电路端到端主频 A/B（参考产物 vs 我们的产物），先量出量级。
4. **M3 语义档案**：接口文档 §9 的 U-L1…U-L14 **必须实验后才写死**（`load`/`store` 的地址单位是地基）。
5. **M2 差分宿主**：先用「参考产物 vs 参考产物」自比对证明宿主可靠，再接我们的产物。
6. **判据入口**（§7.5）：全语料编译 → 逐 tick 差分 → 覆盖率矩阵 + 回退率；游戏内部分标 slow/manual。
7. **主频/bench 层重写**：`tools/freq/` 包 + `hook.log` 迁移 + per-call `impl` 行。与 spike 抢同一份
   `tools/compile_hook.c` → **串行（spike 先、freq 后）**，锁文件 `work/tmp/locks/compile-hook.lock`。
8. **Steam 更新后的重验脚本**（§10 的 5 条断言，按符号名定位、不硬编码地址）。

### 待定

- benchmark 协议与性能目标：暂缓（测法已定，主设计 §7.3）。
- 主设计 §11 的 U1–U4、U7：未动。U5（内建清单完备性）已有权威来源（`compile_real.dll` 的 `.rdata` 字面量池），
  待提取成表（接口文档 §2/U-L13）。

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
- 主设计 `docs/specs/2026-10-15-simplex-llvm-compiler-design.md`（产物契约 T1–T5 闭合；§8 已指向接口冻结版）。
- **接口冻结版 1**：`docs/specs/2026-10-15-simplexc-interfaces.md`（§8 的 7 项 + §9 未定项 U-L1…U-L14）。
  2026-10-17 就地更正 4 处与代码/实测不符处：装配器签名 `ExternResolver&`、`AsmStatus` 补 `BadObject`+`TooLarge`、
  桩长 12 字节、`impl` 词表 = `reference | ours | fallback | ours_failed`（hook.log `impl` 行含 reference）。
- 主频层 `docs/specs/2026-10-15-freq-measurement-design.md`（5 项决策已确认；实现未开始）。
- 代码落库 `reversing/simplexc/`；LLVM 固定 **23.1.3（官方 win64 包）**，链接方式见 `facts.md` §构建与 LLVM。

**M0 观测设施：已完成**（2026-10-16）——98 份语料 + 含 `entry` 的 manifest + 54×98 覆盖率矩阵；
判据 `python simplexc/tools/recompile_corpus.py --check` rc=0（98/98 归一化后逐字节相同）。

**M1 spike：已完成**（2026-10-17）——装配链跑通、游戏内 JIT 执行过（`impl=ours`、seq 006、游戏没崩）。
⚠️ 游戏里那个 blob 来自 **g++ 目标文件**；LLVM 来源的 blob 从没在游戏里执行过（M1.5 顺手闭合）。

**M2 差分宿主：已完成**（2026-10-17）——参考产物自比对通过（005/006/120；判据 `host/selftest.py`）。
未解决：假状态保真度只到「零填 + 数组头」，解引用游戏指针的电路（001 那类）跑不了。

### 当前批次（2026-10-17 接手，按此顺序）

1. **接口对齐（文档已改，代码待跟）**：`assemble()` 的 `max_blob` 超限要返回 `TooLarge`（现在错用 `BadObject`）；
   宿主对「地址表外 + `VirtualQuery` 已映射」的 movabs 要硬错误（现在只统计）；
   `compile_hook.c` 的 `impl` 行改成 per-call（含 reference）+ 词表统一。
2. **构建可复现（当前最大工程债）**：`simplexc/tools/build.py` 不存在 → 换机器无法重建 `tcblob` / `llvm_prog` /
   `compile_hook.dll`；接口冻结时承诺的 `simplexc/README.md` 与各子目录 README 一个都没写。
3. **M1.5 收益探针**：从已下载的 `clang+llvm-23.1.3…tar.zst` 多解出 `bin/clang.exe`（**别再重下**）→
   006 的 C 等价物 → `clang -O2` → `tcblob` 装配 → **先用 M2 宿主验逐 tick 等价**、再比吞吐（同时报静态指令数）
   → 顺带让 **LLVM 来源的 blob 在游戏里真正执行一次**。
4. **壳的失败路径与熔断**（做 A/B 之前必须补）：`ours` 失败现在在 `compile_hook.c` 早返回 → 漏记 manifest 与
   `error` 事件；壳没有 VEH、没有 ours 超时；接口 §8 的错误分类没落地。与 1 里的 `impl` 行同文件 → 串行。

### 下一步（已决定）

1. **假状态保真度**：让 001 那类电路也能在宿主里跑（现在写地址 0）——M3 全语料差分的前置。
2. **M3 语义档案**：接口文档 §9 的 U-L1…U-L14 **必须实验后才写死**（`load`/`store` 的地址单位是地基）。
3. **定 MVP 必须支持的构造集合**：以 `coverage.tsv` 的 98/98 构造为下界，加上 stdlib 前缀里那 6 个，交负责人确认。
4. **前端 + Lowering**（M3 主体）：按接口冻结版 1 实现，产物先接离线 CLI，再接壳。
5. **判据入口**（§7.5）：全语料编译 → 逐 tick 差分 → 覆盖率矩阵 + 回退率；游戏内部分标 slow/manual。
6. **主频/bench 层重写**：`tools/freq/` 包 + `hook.log` 迁移 + per-call `impl` 行。与壳抢同一份
   `tools/compile_hook.c` → **串行**，锁文件 `work/tmp/locks/compile-hook.lock`。
7. **Steam 更新后的重验脚本**（§10 的 5 条断言，按符号名定位、不硬编码地址）。

### 待定 / 未解

- `entry`（`Result[2]`）的用途仍只知「相对机器码起点的偏移」；参考产物 entry 自己 push 全部寄存器。
- 产物里 **75300 处非 image 空间 movabs**（大整数常量、`0x232a…` 游戏地址）跨 boot 相同的机制没查。
- `campaign/` 的 92 份没编译过（可能是片段，编译失败会 abort 整个进程）。
- 字面量的 `'…'`（单引号）与模板插值的语义未定（`docs/notes/2026-10-16-m0-coverage.md` 有单列）。
- benchmark 协议与性能目标：暂缓（测法已定，主设计 §7.3）。
- 主设计 §11 的 U1–U4、U7：未动。U5（内建清单完备性）已有权威来源（DLL 的 `.rdata` 字面量池），待提取成表。

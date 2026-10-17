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
- **当前没有前端、没有 IR**：`frontend/`、`lower/` 只有 README。

**已完成**：
- **M0 观测设施**：98 份语料 + 含 `entry` 的 manifest + 54×98 覆盖率矩阵。
- **M1 spike**：装配链跑通（g++ 来源的空程序，728 B）。
- **M1 缺口已闭合**：clang 来源的产物在游戏进程里跑到过协议主函数（`manifest.tsv` seq 007，
  `impl=ours`，标记文件两行）；同一次触发随后崩，根因是**用过期地址部署**，非逻辑错（见 `facts.md`）。
- **M2 差分宿主**：`<blob>.stubs` sidecar 让我们的产物也能跑同一驱动；`selftest.py --entry`。
- **装配器**：`.bss` 无原始数据节（产物可复现）、桩/槽分两趟定址（`__imp_` 被桩块挤歪），
  返回前自校验 sidecar ↔ 产物字节。
- **工具链**：`build.py`（一键构建）、`check.py`（4 步判据入口）、`verify_game_contract.py`（§10 重验）、
  `gen_addrs.py` + `equiv_build.py`（现取地址 → clang -O2 → 装配 → 双跑宿主 → 逐 tick 比对）。
- 壳：失败路径与熔断（ours 失败记账 / §8 分类 / 超时 / VEH / per-call `impl`）。

**已终止：M1.5 手写等价物验证收益**（2026-10-17，负责人决定）
终止理由：手写 C 等价物的成本随电路复杂度爆炸；能体现优化的电路（每 tick 数百条指令）翻不动，
翻得动的（单与门、计数器）比值又被协议轮询与 `Sleep` 粒度淹没。**保留**的结论只有宿主相对数字
（直译慢 ~25%、热状态进寄存器快 ~2.95×）——「换编译器在游戏里能提速」**没有证据**，这是已知边界。
现有资产不浪费：`equiv_cc_and.c` / `probe_006.py` / `equiv_build.py` 仍是 M3 的回归靶子与比对工具。

### 下一步（已决定，按顺序）

1. **M3.0 垂直切片**（进行中）：靶子是 **RV32IM 电路**（设计 §9 已定），已落盘为稳定资产
  `simplexc/corpus/rv32im/{rv32im.si,rv32im.bin,README.md}`（10369 行 / **532 组件** / `mode_refresh()` 6252 行 /
  参考产物 219088 B、`entry=0x215B`），不再依赖会被覆盖的 `work/hook/`。
  目标：程序吃这份 `.si` → 生成 LLVM IR → `default<O2>` → 目标码 → 装配 → 宿主逐 tick 全等。
  **成功标志 = 第一份机器生成的产物与参考产物逐 tick 全等**，不是收益。
  先交「构造清单 + 语义判定方案 + 验收判据草案」把前端范围钉死再写代码（`m3-0-scope` 在跑）。
  ⚠️ 已知难点：`com_custom` 33 个 → 含子元件层级，不能只做平铺。
2. **语义档案落地**：接口 §9 的 U-L1…U-L14 里被 M3.0 用到的那几条必须先实验后写死
   （`load`/`store` 的地址单位是地基）。清单由第 1 步产出，实验进 `simplexc/dossier/`。
3. **主频/bench 层重写**：`tools/freq/` 包 + `hook.log` 迁移 + per-call `impl` 行消费。
   与壳抢同一份 `tools/compile_hook.c` → 串行，锁 `work/tmp/locks/compile-hook.lock`。

### 待定 / 未解

- 游戏内主频收益量级：M1.5 终止后**不再用「手写等价物 + 游戏内」这条路**量它。
  将来若要量，应等 M3 前端能直接吃真实 `.si` 之后再开（那时热点电路才翻得动）。
- `entry`（`Result[2]`）的用途仍只知「相对机器码起点的偏移」。
- 产物里 75300 处非 image 空间 movabs（大整数常量、`0x232a…` 游戏地址）跨 boot 相同的机制没查。
- `campaign/` 的 92 份没编译过（可能是片段，编译失败会 abort 整个进程）。
- 字面量 `'…'`（单引号）与模板插值的语义未定（见 `docs/notes/2026-10-16-m0-coverage.md`）。
- 参考产物的热环没定位到（要 RIP 采样，不猜反汇编）。
- M3 剩余：MVP 构造集合定稿、前端 + lowering 主体、主设计的 §11 U1–U4/U7。

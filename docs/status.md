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

1. **M3.0 范围已定稿**（`a592483` + `simplexc/dossier/m3_0_constructs.tsv` 140 条 + notes）：
   MVP 134 条构造覆盖**全部代码行 5282/5282**；组件模板 5053 行 = **95.7%**，归一化后只有
   245 种代码行形状（top-10 覆盖 63.7%）；协议骨架只 **229 行**。
   **可达闭包 = 10 个名字 / 5090 代码行**（`decls.py` strict 口径；dossier 的「19 个可达」是把
   `def name(` 声明头当调用，多算 9 个，已在本条更正），加顶层骨架 91 行 → M3.0 规模 **5181 行**。
   **前端第一刀已落地**：`simplexc/frontend/decls.py`（词法 + 顶层声明表 + 函数体只配平不解析），
   99/99 份语料 exit 0、代码行覆盖 100%，`:154` 那个多余 `)` 只记警告。
   `com_switch_bit`(42) 与 `com_switch_word`(40) 两种占全文 51%；`com_probe_wire_bit`(48) 与
   `com_custom`(33) **本体是空块、0 行代码** —— 子元件层级不构成额外 lowering 工作量（更正我先前的判断）。
2. **纪律已定：前端不做 eager 解析**。只解析可达 def（调用图闭包），不可达 def 只做花括号配平跳过并记账。
   依据：参考实现自己就是按需分析（8 个最小片段实测，`docs/notes/2026-10-18-m3-0-scope.md` §7.1）；
   `rv32im.si` 有 24 个不可达 def、其中 82 行含我们暂不支持的构造（`@Size`、`type_of`、`'…'`、
   以及 `:154` 一个多余的 `)`）。这是对接口 §8 的解释，**不改接口文档**。
3. **宿主器材已能跑 RV32IM**（`1b78f83`）。根因不是地址表而是**映射尺寸**：`reset_sim` 每 tick
   整块拷内存镜像（1 号组 `memory_copy(…, 134221824)` = 128 MB + 4096），宿主原先按「地址」给固定 1 MB；
   行尾注释的 `c_alloc(134217728)` 也不对（差 4096 字节就崩）。现按 `memory_copy`/`memory_clear`
   实参算区间（RV32IM 合计 **278.5 MB / 6 段**）。
   **M3 判据基线已建立**：参考 vs 参考 10 tick → `tick 10/10 · 逐 tick 全等=是 · 仿真真在工作=是`。
4. **语义档案：已定 4 条，其余跟着 lowering 走**（不再串行做实验）。
   已定：`U-L1`（load/store 字节地址、无对齐、窄写只写该宽度）· `U-L7`+运行期复核（常量按符号性、
   运行期一律 `zext(trunc_W)`）· `U-L8`（`>>` 永远 lshr、移位量按 W 取模、除零不 trap、`asr` 饱和）·
   `/`、`%`（看被除数类型，除数不参与；折叠窄常量零扩展是参考实现的不自洽，要复刻）。
   剩余 `U-L2`（数组下标）/`U-L4`（struct）/`U-L5`（enum 宽度）/`U-L10`（字符串）/`U-L12`（min/max）/
   `U-L14`（`in_scope`）—— **写一条验一条**。
5. **全链路已打通（M3 首个里程碑）**：`decls.py` → `expr.py` → **`lower/ir_build.py`** → LLVM →
   `tcblob`。吃 `006.si` 生成的产物与参考产物 **逐 tick 差异 0 条**（10 tick、页哈希/`sim_cycle`/extern 全同），
   **全程没有手写 C 参与**。前端判据：`mode_run` 覆盖 1257/1257 = 100%、AST 保真（`src_line`、
   cast 带目标类型、字面量不折叠）、破坏性输入报错带行列号。
   接口级更正：`RelocMode` 由 `Static` 改 **PIC**（`7b6ae4c`，Static 会生成指针内部全局的 ADDR32，
   与 §4-2 互斥；产物要 memcpy 到未知 JIT 基址）。
6. **下一步 = 让 rv32im 装配起来**：`--list-unsupported` 250 条里根因只有两类 ——
   **38 条三目** + **2 条空 struct**（其余 208 条是连锁）。补完这两个就能拿到 M3 的真验收面
   （rv32im 的逐 tick 比对，006 只覆盖 229 行协议骨架）。
7. **主频/bench 层重写**：`tools/freq/` 包 + `hook.log` 迁移 + per-call `impl` 行消费。
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

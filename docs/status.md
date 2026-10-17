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
- **接口权威 = `docs/specs/2026-10-15-simplexc-interfaces.md`（冻结版 1.1 + RelocMode=PIC 更正）**；
  主设计 `docs/specs/2026-10-15-simplex-llvm-compiler-design.md` 只定分区与纪律。
- 主频层 `docs/specs/2026-10-15-freq-measurement-design.md`（架构定稿 + 5 项决策已确认；
  **旧实现存在但没分层**：`tools/read_freq.py` / `watch_freq.py`（566 行）/ `bench_rewrite.py` 三份采样口径互不相同，
  设计要求的 `tools/freq/` 包重构**未做**，正在做 —— 它是 M4 的前置，之前被漏掉。
- 代码落库 `reversing/simplexc/`；LLVM 固定 **23.1.3**。

**已完成**：
- **M0 观测设施**：98 份语料 + manifest + 覆盖率矩阵。
- **M1 spike / M2 差分宿主**：装配链、`.stubs` sidecar、`selftest --entry`、壳的失败路径与熔断。
- **装配器**：`.bss` 无原始数据节、桩/槽两趟定址、返回前 sidecar 自校验。
- **M1.5 已终止**（负责人决定）：宿主内直译慢 ~25%、热状态进寄存器快 ~2.95×，**游戏内无证据**。
- **语义档案（实验定死）**：`U-L1`（load/store 字节地址、无对齐、窄写只写宽度）·
  `U-L7`+运行期复核（常量按符号性、运行期一律 `zext(trunc_W)`）· `U-L8`（`>>` 永远 lshr、移位量按 W 取模、
  除零不 trap、`asr` 饱和）· `/`、`%`（看被除数类型；折叠窄常量零扩展是参考实现的不自洽，要复刻）。
- **前端**：`decls.py`（词法 + 顶层声明 + 函数体只配平）→ `expr.py`（语句 + 表达式 AST，`mode_run` 覆盖 100%，
  AST 保真：`src_line`、cast 带目标类型、字面量不折叠）。
- **判据补强**：每 tick 对每个映射段算 sha256（`regions` 字段）+ `selftest.compare` 纳入比较；
  `--seed-reset-data seq|xorshift` 给 `#RESET_DATA_*` 灌图案 —— 272 MB RAM 镜像才真的被读写。
  **强判据下 rv32im 仍逐 tick 全等**，且因此抓出并修掉一个 `memory_copy` 方向反的 bug。
- **freq 层重构完成**（M4 前置）：`tools/freq/` 11 个模块（`probe`/`variants`/`products`/`sampler`/
  `report`/`driver`/三个 CLI），`watch_freq.py` 566→25 行、`bench_rewrite.py`→27 行薄壳；聚合统一 median、
  `hook.log` 迁到 `work/hook/`（`resolve()` 写跟随读，窗口期不分裂 `cfg`/`opt`）；39 单测。
  ⚠️ 游戏相连路径全部**未验**（真按键/真内存读/preflight 真分支），留给 M4。
- **pi-lens 清零**：`ir_build.py` 17 条 → 0（`lens_diagnostics` 权威入口确认），产物 md5 逐字节不变。
- **M3 达成（宿主面）**：`lower/ir_build.py`（AST → LLVM-C → 目标码 → `tcblob`）。
  **006 与 rv32im 都与参考产物 10 tick 逐 tick 全等（差异 0 条）**；rv32im 产物 **58612 B vs 参考 219088 B**。
  全程没有手写 C 参与。接口级更正：`RelocMode` `Static` → **PIC**。

### 下一步（已决定，按顺序）

1. **M4 游戏内主频对照**（前置已全部就绪）：把 rv32im 产物部署进游戏、切 `ours`，多轮取中位数、同会话交替
   `reference`/`ours`。⚠️ **游戏内必须重新捕获 rv32im 源码**（产物里的地址只对捕获它的进程有效）；
   需要负责人配合起游戏、让 RV32IM 持续跑。
2. **M4.5 前端换 C++17**（M5 前置，见主设计 §9）：把 `frontend/` + `lower/` 从 Python 原型搬成 C++17。
3. **M5 全量语料**：98 份逐份跑「能否装配 + 逐 tick 是否全等」，出一张覆盖表。
4. **优化（M3 §11 U1–U4/U7）**：现在全 i64 + `alloca` + `align 1`，我们 708 条指令 vs 手写 C 版 508 条 —— 明确有余量。

### 待定 / 未解

- 空 struct 的 `packed { i8 }` 1 字节占位是 U-L4 未定下的临时选择。
- 剩余语义：`U-L2`（数组下标/越界）、`U-L4`（struct 布局）、`U-L5`（enum 底层宽度）、
  `U-L10`（字符串字面量编码）、`U-L12`（min/max 类型规则）—— **写一条验一条**。
- 游戏内真环境未验（真 `Sleep`/时钟、游戏设的 `target_cycle`）。
- `entry`（`Result[2]`）的用途仍只知「相对机器码起点的偏移」。
- `campaign/` 的 92 份没编译过；参考产物的热环没定位到（要 RIP 采样）。

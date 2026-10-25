<!-- 整体重写，禁止追加。≤80 行。
     一次性结论 → notes/；被复用过的结论 → facts/。具体事实不在这里复制。 -->
# 状态

在追：**M5**。目标是把 `compile.dll` 换成自研实现（Simplex → LLVM IR → 装配成游戏可 JIT 的 blob）。
正确性判据：同一份 `.si`，我们的产物与参考产物**逐 tick 状态等价**。
验收边界（2026-10-18 负责人确认）：沙盒模式只看 RV32IM 电路。

- 接口权威 = `docs/specs/2026-10-15-simplexc-interfaces.md`（冻结版 1.4）；主设计 = `…-simplex-llvm-compiler-design.md`。
- 代码在 `reversing/simplexc/`；LLVM 固定 23.1.3。

## 判据设施：difftrace（2026-10-25 建成）

`simplexc/tools/difftrace/`，设计见 `docs/specs/2026-10-25-difftrace-design.md`。
用 Unicorn 跑 blob，**记录所有 store，按 tick 折叠成内存状态再比**——判据是状态，不是轨迹。
所以 DCE 掉的死代码、写几次、写的顺序都不影响结论。

| | `host.py`（旧） | `difftrace`（新） |
| --- | --- | --- |
| 每 tick | 200 ms（sha256 148 + 双遍全扫 54） | 几十 µs（只记被写的字节） |
| rv32im 100 tick | ~20 s | **5.4 s**（其中 5 s 是一次性启动的 128 MB `rep movsb`） |

`host.py` 保留作 golden：difftrace 的结论与它一致才算可信。

**自校准通过**（rv32im，`--seed seq --target 100`）：参考产物 vs 我们的产物，最终状态 **equal**、
逐 tick **0 差异**、两边折叠后各 **1616** 条字节写（原始 38169 vs 11613）。

停机判定必须精确：监视 `sim_running` 被写 0 的那一刻 `uc_emu_stop`。早期按「每 1000 条指令采样」
判完成，两边切点深度差 8 倍，凭空造出 688 字节假差异。

## 语义 bug 修复（2026-10-24）

由 sb MIPS 电路暴露，四个都是「我们的实现与参考语义不一致」：

| # | 位置 | 语义面 |
| --- | --- | --- |
| 1 | `sema/types.cc` `storage_bytes` | `N>32` 的存储宽度是 8 字节 |
| 2 | `sema/analyze.cc` `arith_ty` | 字面量多态要铺给**所有**算术运算符 |
| 3 | `codegen`/`types` 移位量掩码 | `pow2_ceil(storage_bytes(W)*8)-1`，不是 `W-1` |
| 4 | `frontend/parser.cc` `prec()` | `+ - \| ^ &` 同级左结合、`* / % << >>` 同级 |

sb 验收：参考 `hook_out_012.bin`（entry `0x2142`）vs 我们，50 tick、51 450 个变化页逐字节 0 差异。
过程：`docs/notes/2026-10-24-sb-mips-precedence.md`；优先级表：`docs/facts/semantics.md` §U-P。

98 份语料（10 tick）：可装配 75 · 全等 **39** · 只差 extern **34** · diff **2** · 装不出 23。

## 下一步

1. **把 difftrace 接进 `check.py`**：现在它是独立工具，判据入口还是 `coverage.py` + `host.py`。
   接入后 rv32im 的回归从 20 s 降到 5 s，且有「第一个不同 tick + 字节明细 + 写它的 pc → 源码行」的定位能力。
2. **`--seed` 口径统一**：`xorshift` 会让 rv32im 在 `cyc=13` 报「非法访存」（`#RESET_DATA_*` 是游戏里的
   真实内存镜像，宿主没有它，随机字节填进去 CPU 就跳到垃圾上）。`none`/`seq` 都能跑满 target。
   `coverage.py` 默认 `seq` 是对的，手工跑别再传 `xorshift`。
3. **语义未测项**（参考行为已测出、实现未验）：一元 `- ~ !` 的档位、`?:` 的 `else` 只吃一个紧单位且左结合、
   **窄类型操作数让整条运算在窄位宽求值**（`(U8 1)+255`=0 而 `(U32 1)+255`=256）。
4. **M5 收尾**：18 份卡 sema 的三类（`hex`/`str`/`bin`）是刻意不做；5 份卡装配期 `kernel32!memcpy` 缺符号；
   2 份 `diff`。这些决定 M6 能不能关回退。

## 待定 / 未解

- `work/tmp/` 里有约 1.9 G 历史实验产物（`diffclass2` 339 M、`prio6` 270 M 等），不是本次产生的，未清。
- 逐 tick 界标没有真解：`cycle` 是 VM 全局槽（不是内存写），看不见。`+2567` 是 tick 体内第 9/27 条写，
  段末停在 tick 中途；换成 `+2908`（体内最后一条写）才对齐边界。主判据（终态）不依赖界标。
- difftrace 只比 `target` 之内；target 之外的收尾块不比。
- `blob_baseline.py:409` 在「只有 blob_len 变、无字节区间」时抛 `IndexError`（既有 bug，未修）。
- 已知未修差异：顶层非声明语句被整体丢掉 · 参考对 `return a) + 1` 静默吃掉后面的一切 · 空数组的指针值未对照。

## 环境

- `build/` 是共享的：多写者按目录切分，同一时刻只让一个写者跑判据入口。
- `python simplexc/tools/build.py` 默认目标**不含** `simplexc_cli`，改了编译器要再跑 `build.py simplexc_cli`。
- difftrace 依赖不入 git：Unicorn 2.1.4 与 Capstone 5 的 C SDK 来自各自 PyPI wheel，
  准备步骤见 `simplexc/tools/difftrace/README.md`。

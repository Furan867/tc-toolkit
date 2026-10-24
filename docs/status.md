<!-- 整体重写，禁止追加。≤80 行。
     一次性结论 → notes/；被复用过的结论 → facts/。具体事实不在这里复制。 -->
# 状态

在追：**M5 收尾**（分层重构 M5.5 已于 2026-10-24 完成）。

目标：把 `compile.dll` 换成自研实现（Simplex → LLVM IR → 装配成游戏可 JIT 的 blob）。
正确性判据：同一份 `.si`，我们的产物与参考产物**逐 tick 状态等价**。
验收边界（2026-10-18 负责人确认）：沙盒模式只看 RV32IM 电路。

- 接口权威 = `docs/specs/2026-10-15-simplexc-interfaces.md`（冻结版 1.4）；主设计 = `…-simplex-llvm-compiler-design.md`；
  主频层 = `…-freq-measurement-design.md`。代码在 `reversing/simplexc/`；LLVM 固定 **23.1.3**。
- 重构全过程、每个决策与踩过的坑：**`docs/notes/2026-10-24-refactor-plan.md`（唯一入口）**。

## M5.5 分层重构：完成

目录就是设计 §8 的形状：`frontend/ sema/ codegen/ runtime/ assemble/ shell/ cli/`。四条硬要求逐条兑现：
sema 不发 LLVM 指令 · codegen 不查 AST/不推类型/不决定实例化 · 无 save/restore · 错误三轴从第一层带出。

`check.py` 6 PASS · `blob_baseline` 98/0 · `codegen_selftest` 10/10 · `analyze_selftest` 24/24 · `hir_selftest` 全过。

## 语义 bug 修复（2026-10-24）

由 sb MIPS 电路（`sb_TCMIPS-SDLPAL.si`，9067 行）结果不对暴露。四个都是「我们的实现与参考语义不一致」：

| # | 位置 | 语义面 |
| --- | --- | --- |
| 1 | `sema/types.cc` `storage_bytes` | `N>32` 的存储宽度是 8 字节，不是 `ceil(N/8)` |
| 2 | `sema/analyze.cc` `arith_ty` | 字面量多态要铺给**所有**算术运算符，不只是 `+` |
| 3 | `codegen`/`types` 移位量掩码 | `pow2_ceil(storage_bytes(W)*8)-1`，不是 `W-1` |
| 4 | `frontend/parser.cc` `prec()` | `+ - \| ^ &` 同级左结合、`* / % << >>` 同级 |

**sb 验收**：参考 `hook_out_012.bin`（entry `0x2142`）vs 我们，各跑 50 tick，**51,450 个变化页逐字节不同 0 个**，
`sim_cycle`/`alive`/`extern` 逐 tick 全等。根因是 `x & 0xFFFFFFFF + 1` 被解析成 `x & 0`，计数器自增恒 0。
过程与弯路：`docs/notes/2026-10-24-sb-mips-precedence.md`。

**重记账**（10 tick · 非平凡图案）：可装配 75 · 逐 tick 全等 **39**（改前 30）· 只差 extern **34** · diff **2** · 装不出来 23。

## 语义实测方法（新的默认做法）

优先级/结合性这类语言语义**不再靠猜**：写「两种结合给出不同值」的判别式片段，用参考编译器编，
从产物里读 `movabs`+`b9 imm` 的立即数。优先级表见 `docs/facts/semantics.md` §U-P。
参考编译器在独立进程里**是确定的**（同片段连编两次 sha 相同）；之前「产物会变」是宿主运行时抖动，不是编译器。

## 下一步

1. **优先级表还差三块实测**（参考行为已测出，我们的实现未验证）：
   - 一元 `-` `~` `!` 的档位（实测在最紧之上）；
   - `?:` 的 `else` 只吃一个紧单位、且左结合（与 C 相反）；
   - **窄类型操作数让整条运算在窄位宽求值**（`(U8 1)+255` = 0 而 `(U32 1)+255` = 256）—— 这条可能影响全部窄类型算术。
2. **M5 收尾**：18 份卡 sema 的三类（`hex` 13 + `str` 3 + `bin` 1）是刻意不做，补了等于新增能力；
   5 份卡装配期 `kernel32!memcpy` 缺符号（不是编译问题）；2 份 `diff`（096 extern / 098 段哈希）在基线里本来就是 diff。
3. **回退率按三轴统计**：shell 已能拿到 `kind`，先跑一轮得基线，那是 M6 关回退的量化依据。

## 待定 / 未解

- 壳在 `compile_real.dll` 加载失败时不写 `out[3]`（保留 0xCC 哨兵）；要不要自造失败 Result 待定。
- `simplexc_core.cc` 的 `catch(...)` 仍折成 `InternalError` → 会转交；今天不可达，但留着就是一处静默降级。
- `codegen_selftest` 的 `.irkeys` 只能断言「必须出现」，写不出否定断言。
- `blob_baseline.py:409` 的 `print_diff` 在「只有 blob_len 变、无字节区间」时抛 `IndexError`（既有 bug，未修）。
- `facts/semantics.md` 的 U-L1 说 `N>32` 是 8 字节访存，但 IR 上是 `i40` —— 这层映射只在 `codegen.cc` 注释里。
- 已知未修差异：顶层非声明语句被整体丢掉 · 参考对 `return a) + 1` 静默吃掉后面的一切 · 空数组的指针值未对照。
- `entry`（`Result[2]`）用途仍只知「相对机器码起点的偏移」；参考产物的热环没定位到（要 RIP 采样）。

## 环境

- **`build/` 是共享的**：两个写者同时跑 `build.py`/`check.py` 会互相覆盖产物；多写者按目录切分，
  且同一时刻只让一个写者跑判据入口。
- `python simplexc/tools/build.py` 的默认目标**不含** `simplexc_cli`，改了编译器要再跑 `build.py simplexc_cli`。

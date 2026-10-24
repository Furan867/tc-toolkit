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

| 要求（设计 §3.2–§3.4） | 判据 |
| --- | --- |
| sema 不发任何 LLVM 指令 | `grep -c LLVM simplexc/sema/` = 0 |
| codegen 不查 AST/源码、不做类型推导、不决定实例化 | `grep -rn 'frontend/' simplexc/codegen/` 为空；`convert` 从 16 处删到 4 处且都有理由 |
| 无 save/restore 可变状态 | `grep -c 'save_state\|restore_state\|FnState' simplexc/codegen/` = 0 |
| 错误三轴从第一层带出 | `sema/`+`codegen/`+`frontend/` 共 168+ 个抛点全是 `CompileError{resp,stage,span,detail}`；`Unsupported`/`CompileFail` 在产品代码里清零 |

**判据（全绿，可逐条复跑）**：

- `python simplexc/tools/check.py` → 6 步全 PASS（含 `blob_baseline` 与 `hook_offline` 11 个 case）。
- `python simplexc/tools/blob_baseline.py --verify simplexc/tools/baseline` → **98 份产物逐字节相同**（掩掉 stub/slot 槽位）。
- `python work/tmp/s2c/dual.py --all-corpus` → 旧 AST 路径与新 HIR 路径 **75/75 相同**（blob + IR 逐函数）。
- `build/analyze_selftest.exe` 24/24、`build/hir_selftest.exe` 全过、`build/codegen_selftest.exe` 5/5。

**重记账（2026-10-24，`docs/notes/2026-10-24-m5-recount.md`）**：98 份 · 10 tick · 非平凡图案 ——
可装配 **75** · 逐 tick 全等 **30** · 只差 extern **31** · 装配成功但不等 **14** · 装不出来 **23**。
**与重构前逐 id 98/98 一致，变了 0 份** —— 这比重构只比 blob 更强，它包含宿主里逐 tick 跑。

## 下一步

1. **M5 收尾**：18 份卡在 sema 的语料全是**刻意不做**的三类——`hex` 13 + `str` 3 + `bin` 1 +
   `ui_set_instruction` 1（它们在基线里本来就不出产物，补了等于新增能力，会污染「产物逐字节相同」判据）；
   另有 5 份卡在装配期 `kernel32!memcpy` 缺符号（它在 msvcrt/ntdll，属符号解析问题，不是编译问题）；
   以及 14 份 `diff`（A′/C′/D′ 残部）。这些决定「能不能关掉回退」（M6）。
2. **回退率按三轴统计**：shell 现在能拿到 `kind`，先跑一轮得到基线（分 `UserError`/`UnsupportedConstruct`/`InternalError`），
   那是关回退的量化依据。
3. 语义未验项（`U-L4` 偏移/padding、`$` 写回时机、`U-L5` 显式宽度语法、`Str == Str` 编译期折叠、
   `@Type` 值形实参）—— 判据与缺口见 `docs/facts/semantics.md`。

## 待定 / 未解

- 壳在 `compile_real.dll` 加载失败时不写 `out[3]`（保留 0xCC 哨兵）。那是部署错误、原版无法转交，
  要不要自造失败 Result 待定。
- `simplexc_core.cc` 的 `catch(...)`（非 `std::exception` 的前端值）仍折成 `InternalError` → 会转交。
  今天的前端都抛 `CompileError`，所以这条不可达；但留着就是一处静默降级。
- `codegen_selftest` 的 `.irkeys` 只能断言「必须出现」，否定断言（`llvm.trap` 不该出现）写不出来。
- `facts/semantics.md` 的 U-L1 说 `N>32` 是 8 字节访存，但 IR 上是 `i40` ——
  「事实 → IR 宽度」这层映射只写在 `codegen.cc` 的注释里，facts 没有。
- 已知未修差异：顶层非声明语句被整体丢掉 · 参考对 `return a) + 1` 静默吃掉后面的一切 · 空数组的指针值未对照。
- `entry`（`Result[2]`）用途仍只知「相对机器码起点的偏移」；参考产物的热环没定位到（要 RIP 采样）。

## 环境

- **`build/` 是共享的**：两个写者同时跑 `build.py`/`check.py` 会互相覆盖产物；多写者按目录切分，
  且同一时刻只让一个写者跑判据入口。

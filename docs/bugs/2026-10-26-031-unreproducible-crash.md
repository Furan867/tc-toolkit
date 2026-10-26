# 031/032「逻辑试炼」：游戏内崩溃的观察不可复现（原根因判断已推翻）

- 状态：**未修；但原观察已被推翻** —— 没有可修的根因，缺的是重新取证
- 影响：031/032（关卡「逻辑试炼」，`work/hook/error-level/error-level.txt:8`）。不在 98 份语料里，
  所以不影响离线判据；只影响「游戏内能不能跑这一关」

## 现象（2026-10-25 的记录，出处没留下来）

当时 `docs/status.md` 写：031-032 游戏内崩溃（`pc` 未映射、store 0 条），产物 9916 B 而参考 105469 B（1/12），
20 个接口函数（`get_cycle`/`get_component_count`/`check_output`/`get_input`…）没进产物，
根因判到 `sema::Analyzer::run` 只分析程序根。

**这两个数字在仓库里找不到对应文件**：`work/hook/all/hook_out_031.bin` 是 35000 B，
`work/hook/all/hook_src_031.si` 是 64911 B。所以「9916 / 105469」不是这一对。

## 已确认（推翻原结论）

1. **参考同样丢弃未调用的函数**：把 031 里 31 个从未被调用的 `def` 整段删掉再编，
   参考产物 `ref_031.bin` 35000 B → `ref_031_nodefs.bin` 35000 B，`cmp -l` 只差 **4 字节**。
   参考对空源码的产物固定 **8581 B**（`ref_empty.bin`）。
2. **我们的产物没有截断**：`--verbose` 报 `defs=8`（进 AST 的 def 只有 8 个，不是 39/43）；
   `--ir-out` 里 `tc_main` 是完整根：`while true → reset_sim → mode_refresh → 四路 switch（run/refresh/mode_reset/quit）→ mode_run`。
3. **离线跑得通**：`difftrace run --src work/hook/all/hook_src_031.si --bin … --target 100` 无异常；
   `#SIMULATION_STATE` 的 272 个状态地址被我们写到 **271** 个。
4. **3404 B vs 35000 B 是编码密度，不是漏编**：`mode_refresh` 的体在参考占 22351 B（清空该体 → 12649），
   在我们占 1296 B（3404 → 2108）；同一段 282 次 `store()` 我们编成 269 个机器写点。
   参考的体积来自它自己的调用约定与内建辅助 —— 小片段比体积是无效判据。

## 已排除（别再往这些方向修）

- **「未被调用的顶层 def 不进 hir.funcs」**：比 sema 更早 —— 前端就不解析不可达 def
  （`codegen/module_build.cc:360` → `frontend/decls.cc:662 strict_root_lines` / `:690 closure` → `:371`）。
  改 `Analyzer::run` 是空操作。
- **「同名 def 被内建遮蔽」**：探针 `work/tmp/rc031/probe4_shadow.si`
  （`def max(a: Int, b: Int) Int { return 999 }` + 根里 `max(1, 2)`）→ 参考产物与我们的产物**都含 `mov $0x3e7`**，
  都是用户 def 优先。先前的 `shadow_full.si` 用 `def max(a: TestResult, …)` 接 `Int` 实参，
  本来就该选内建，不能当证据。
- **「不可达 def 里的语法错被静默吞掉」**：探针 `probe3_badsyntax.si`（`def unused() None { let x = ) }`）
  → 参考 8604 B **不报错**，我们 960 B 同样不报错。**与参考一致**；它属于设计文档 §3.4「消除静默降级」
  的自我改进目标，不是与参考的差异 bug。

## 顺带发现（真差异，未修，与崩溃无关）

裸顶层语句：`store(...)` 直接写在顶层时**参考接受**（产物 +13 字节），我们报
`顶层这一行既不是声明也不是语句（未支持）`。98 份语料里没有这种形态（根都是 `run_sim: while true { … }`），
所以没暴露过；要修属前端形态补齐，不是崩溃根因。

## 下一步

重新取证「游戏内崩溃」：先找原始观察的出处（哪次会话、哪个 `manifest` / `hook.log`、哪个 `hook_src_NNN.si`）；
找不到出处就在游戏内重测（要 `game.lock`，属游戏内实验）。

## 复现

上各条的命令与产物都在 `work/tmp/rc031/`（临时目录，会被清）：
`ref_031.bin` / `ref_031_nodefs.bin` / `A.ll` / `probe3_badsyntax.si` / `probe4_shadow.si`。

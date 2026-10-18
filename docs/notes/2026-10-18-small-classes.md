# 小类逐个吃（2026-10-18）：const # / clz / ror

**一句话**：本轮吃下 3 个小类 —— `const #`（11 份）、`ror`（2 份）装配通过，`clz`（2 份）装配
通过；口径从 **16 可装配 / 15 全等 / 1 diff / 82 cli_fail** 变成 **29 / 18 / 11 / 69**。

判据现场（全部原始输出在本次会话里跑过）：

```text
build/simplexc_cli.exe --src simplexc/corpus/src/008.si --out …   # 改前 UNSUPPORTED 未定义的名字 #CORRECT_OUTPUT
                                                                 # 改后 blob 2164 B
python simplexc/tools/check.py                    → 5 步 5 PASS · 0 FAIL · 0 SKIP
python simplexc/tools/m5/coverage.py              → 29 可装配 / 18 全等 / 11 diff / 69 cli_fail
ast-grep（pi-lens 全部 298 条规则，见 2026-10-18-ir-build-lint.md 的入口）→ lower.cc/lower.hh 0 条
```

## ① `const #CORRECT_OUTPUT`（11 份：008/010/012/014/016/018/022/032/040/056/067）

- **最小复现**：`work/tmp/small/const_hash.si`（改前 `UNSUPPORTED: 未定义的名字 #CORRECT_OUTPUT
  @ 第 10 行`、改后装得出来）。
- **产品判错位置**：`lower.cc` 的 `e_IdentExpr` 兜底那句（报的是**引用点**的行，不是声明行）。
- **缺功能，不是缺陷**：`const #CORRECT_OUTPUT = [U1 1,0]` 声明在 `run_sim`（程序根）体里，
  而引用它在**嵌套 def `check_output` 的体**里，该 def 的调用点又在**顶层 def `mode_run`** 里
  （008:347）—— 那时 const 的词法作用域不在 `scopes_` 栈上，`lookup` 查不到。
- **修法**：把 `collect_root_decls` 已经收到的程序根体 `const #`（`RootDecls::consts`）与
  `root_defs_` 同档处理 —— 值也能按名取到（`root_const_decls_` + 按**声明节点指针**备忘的
  `root_const_values_`，见 `s_VarDeclStmt` / `root_const_value`）。备忘按指针而非按名字：
  程序根自己那句声明与外部引用点只求值一次、只出一个私有全局常量。
- **规模**：`lower.cc` +25 行、`lower.hh` +9 行（含注释）。
- **结果**：11 份全部从 cli_fail 变可装配；其中 032/040/067 **逐 tick 全等**，
  008/010/012/014/016/018/022/056 变 `diff`（撞下一类，见「遗留」1）。

## ② `ror`（2 份：178/179）

- **最小复现**：`work/tmp/small/ror.si`（改前 `UNSUPPORTED: 未定义的函数 ror @ 第 9 行`）。
- **参考行为两条路**（`work/tmp/ror-wip.patch` 记的实测，本轮复核其结构）：两个常量操作数时在
  编译期**按 64 位**折叠（`ror(U8 0x81,1)` 给 64）；有一个运行期值时按**操作数位宽**算、旋转量按
  W 取模（`ror(U32 0x80000001,1)` 给 0xC0000000）。
- **缺功能**：`call_named` 里没有这个裸名（内建表还没收）。
- **规模**：`lower.cc` +45 行（`builtin_ror`）。
- **结果**：178/179 从 cli_fail 变可装配（仍 diff，见「遗留」1）。

## ③ `clz`（2 份：131/137）

- **最小复现**：`work/tmp/small/clz.si`。
- **参考语义（本轮实测，新）**：数前导零的范围 = **操作数声明的位宽**，常量与运行期两条路一致。
  探针 `work/tmp/small/clz_probe.py`（参考 DLL 编译 + 宿主读 ui_buffer 渲染出的字符串）：

  | 表达式 | 结果 | | 表达式 | 结果 |
  | --- | --- | --- | --- | --- |
  | `clz(U8 0)` | 8 | | `clz(U4 0)` | 4 |
  | `clz(U8 1)` | 7 | | `clz(Int 0)` | 64 |
  | `clz(U8 0x80)` | 0 | | `clz(U64 0)` | 64 |
  | `clz(U16 0)` | 16 | | `clz(S8 -1)` | 0 |

- **规模**：`lower.cc` +25 行（`builtin_clz` + `ensure_clz_fn`），`lower.hh` +2 行。
  实现 = i64 的 `llvm.ctlz`（第二参数 false）再减 `64 - 位宽`。
- **结果**：131/137 都不再报 `未定义的函数 clz`，但都**撞下一类**：131 撞 `bin`（前缀函数，
  路线 A），137 撞既有的 CompileFail/ICmp 那类 → 本类不减 cli_fail 份数，是链式推进的一环。

## 遗留（本轮**没做**，分类列出）

1. **8 份「段哈希」diff**（008/010/012/014/016/018/022/056）与 178/179：装配成功但逐 tick 不等。
   008/010 的**我方**段哈希逐字节相同（`14ddcdb3…`）、参考侧不同 → 像是「参考写了某段、我们没写」，
   与本轮三类都不同。**没定位到最小复现**。
2. **`array` 15 份**：`var x = array(<Int>, 256)`。`array` 在参考里是 `mac_array`（内建），
   要 arena/分配器 —— 本轮没动（后续与 `grow/shrink/join/empty_array_address` 一起做）。
3. **`random` 14 份**、`bin` 1 份（131）：**前缀里定义**（`stdlib_prefix.si` 的
   `random`/`bin`/`hex`/`str`），归路线 A。注意 `hex 13 份`/`str 3 份` 在语料里被引用的远多于
   当第一位错误的份数 —— 它们都在前缀里，按路线 B 只能逐个手写语义（本轮未做）。
4. **闭包捕获类**（`count`/`input`/`seen`/`over`/`x_pos`/`test`/`memory` 共 12 份）：引用点从
   嵌套 def 体里取**外层 `var`**（024 的 `let count = …` 被 `check_output` 用）→ 要闭包转换
   或改 ABI，按上限规则**不做**。
5. **跨层交接**：`3e30683`（另一个 worker）加了 `NK::BlockStmt`（裸块 `{}`），提交信息写
   「lowering 待补」—— `lower.cc` 里还没有 `NK::BlockStmt` 的分支（`ast.hh` 的注释已写明语义：
   直接内联展开、块内名字出块不可见、允许遮蔽）。那 12 份现在先撞 `array`（15 份里含它们），
   所以这个缺口暂时不显形。
6. **探针的 ours 侧在宿主里崩**（`ror_loop.si` 0xC000001D @entry+0x60E、`clz_loop.si` 0xC0000005
   @entry+0x280），所以 ②③ 只有装配判据、没有逐值对照；`fixsmall/ror_pages_ours/` 里当初也只留下
   tick1 的页 → 崩溃在改动之前。**未定位**，与本轮三类的产物无关（rv32im/006 的判据全过）。

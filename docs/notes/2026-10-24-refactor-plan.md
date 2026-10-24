# 重构方案（M5.5）：lower/ → sema/ + codegen/

状态：方案定稿，待执行。负责人 2026-10-24 确认「按 A（分两步），S1 可以弱，终态必须满足设计文件」。
上游：`docs/specs/2026-10-15-simplex-llvm-compiler-design.md` §3.2–§3.4 / §7.6，
`docs/specs/2026-10-15-simplexc-interfaces.md` §1b（HIR 目标形态）。

## 0. 事实底数（实测，2026-10-24）

- `lower.cc` 3428 行；`Lower`/`Program` 成员函数 144 个 2738 行：纯 sema 31/519、纯 codegen 54/509、
  **mixed 59/1710（62.4%）**；另自由函数 31 个 409 行、`module_build.cc` 整文件 0 LLVM。
- 机械拆分上限 = 19 成员(330) + 31 自由(409) = **739 行 ≈ 21.6%**；其余都触碰
  `V{ll,ty,c}` / `Binding{ptr,…}` / `FnState` / `Closure` / `Capture{ptr,…}` / `Fn`。
- 逐字节回归判据已可执行：98 份全量 **8.5s**，比的是掩掉 `stub/slot` 槽位的 `norm_sha256`；
  raw sha 只在同一次开机内跨进程可比（跨 boot 会变，别当判据）。
  `python simplexc/tools/blob_baseline.py --verify …`。

## 1. 终态（不可打折的部分）

| 要求 | 出处 | 验收 |
|---|---|---|
| sema 不发任何 LLVM 指令；codegen 不查 AST/源码、不做类型推导、不决定实例化 | §3.2 | `grep -c LLVM simplexc/sema/` == 0；codegen 的 include 里没有 `frontend/ast.hh` |
| HIR 六条不变量 | §3.2 | 每条一个 `assert` + 一条最小用例 |
| 无 save/restore 可变状态；递归用子上下文或显式参数 | §3.3 | `grep -c 'save_state\|restore_state'` == 0 |
| 错误三轴从第一层带出 | §3.4 | 失败用例表：责任/阶段/span 可区分 |
| 实例键唯一且规范化，实例集合由 sema 定死 | §1b / §3.2-6 | 同 (def, 实参, 环境) 只出现一次，dump 可见 |

## 2. 关键决策（本方案定的，写进代码注释，不进 specs）

1. **HIR = AST 的去糖投影 + 显式 cast + 已解析引用**，独立节点树（`sema/hir.hh`），不复用 AST 节点。
   理由：不变量 4（隐式转换显式化）需要插节点；codegen 不许看源码，而 AST 带语法糖
   （`#if`、`const #`、`let/var` 区别、体内 `def`）。**不做三地址码/MIR** —— LLVM IR 就是 MIR（§3.2）。
2. **codegen 保留自己的值表示与作用域表**（`V{ll,ty}` 照旧）。§3.3 只说「sema 的 V 不带 ll」，
   没要求 codegen 改值表示。分层后 codegen 的 `V.ty` 从 HIR 读、不从 AST 推。
3. **局部引用用名字**（不引入 slot 编号），让 alloca 顺序/命名不变 → 逐字节判据能兜住。
4. **常量折叠全部留在 sema**：HIR 的 `Const` 节点已折叠（含截断，语义见 `facts/semantics.md`）。
   codegen 见到 `Const` 直接用，不再有 fold 分支。
5. **`#if in_scope` 判定与删子树在 sema**（§3.2 审查更正）；前端只留 `PpIf` 节点。
6. **内建在 sema 解析成 `BuiltinId`**（带 mutating/返回/对齐语义），HIR 里不留源码名字。
7. **实例键** = `<函数名>$<实参规范化串>`，串由类型规范化函数生成（递归：bits/signed/struct 名/array elem）；
   闭包面另带捕获签名（现状键的第 4 位）。规范化函数**唯一一处**，sema 与 codegen 都调它。
8. **业务不双跑**：不保留「旧 lowering 路径 + 新 HIR 路径」并存。迁移点放在最后一次提交，
   中间提交保持 check.py 全绿（新增 `sema/` 与 dump 工具不接线）。

## 2b. HIR 规格（冻结，落码 `simplexc/sema/hir.hh`）

节点集 = AST 的去糖投影。**不含**语法糖节点（`#if`、`const #`、`let/var` 区别、体内 `def` 声明、`if_case` 容器）。

| 类别 | 节点 | 载荷 | 备注 |
| --- | --- | --- | --- |
| 表达式 | `Const` | 位模式 u64 | 已折叠；`ty` 已定宽 |
| | `StrLit` | 串池 id | 字节与 cstring 标志在 `Hir.strings` |
| | `Local` | 名字 | 已绑定到局部声明 |
| | `Global` | 全局 id | |
| | `Capture` | 序号 | 闭包捕获 |
| | `Unary` / `Binary` | op | 结果类型已定（`arith_ty`/`unify` 在 sema 跑完） |
| | `Cond` | 3 子 | |
| | `Call` | callee = `FnId` 或 `BuiltinId` | 不许留源码名字 |
| | `Member` | 字段序号 + 偏移 | 布局由 sema 定 |
| | `Index` | 2 子 | |
| | `Cast` | `{Trunc, Sext, Zext, PtrToInt, IntToPtr, Raw}` | **隐式转换一律走这里** |
| | `ArrayLit` / `CompositeLit` | 子序列 | 字段初值按声明序 + 偏移 |
| | `Template` | literal / expr 交替 | `Str` 插值 |
| 语句 | `Block` `VarDecl` `Assign` `ExprStmt` `If` `While` `Switch` `Break` `Return` | | `Assign` 的 lhs 已是 HIR 左值（`Local`/`Member`/`Index`） |

顶层：

```text
Hir { main: HirFunc; funcs[]; globals[]; externs[]; strings[]; consts[]; structs[] }
HirFunc { key(规范化实例键); name(逻辑名, 如 main / f$u64); cap_index(-1=无捕获); params[]; caps[]{name,ty,index}; body; ret }
HirGlobal { name; ty; init_bits: optional<u64> }   // 初值必须是编译期常量，sema 折好
HirStruct { name; bits; fields[]{name,ty,offset} }
```

**三处规格补漏（2026-10-24 由 S2c 切割清单查出，不加就产物会变）：**

1. **符号名分层**（2026-10-24 实测 `lower.cc:2369/2373/3194` 后改定）：HIR 只给**逻辑名**（`main` / `f$u64`，
   即接口 §1 的实例化名）+ `cap_index`；**产物符号名由 codegen 派生**，规则必须与现状逐字相同：
   程序根 = `tc_main`；顶层 def = `name` 或 `name + "#L" + lo`；有捕获再挂 `"$c" + cap_index`；
   嵌套 def = `owner + "$" + name + "#L" + src_line`。sema 若直接把产物名钉死，等于把产物细节搬进 sema，并且
   `#L` 的选法（`lo>=0` 与否）属于 codegen 的 `ensure_*_fn` 逻辑，搬过去就是隐式耦合。
2. `HirGlobal.init_bits` 与 `Hir.structs[]` 是硬缺口：`make_globals` 要求全局初值是编译期常量，
   `make_types` 今天从 `ast_.structs` 建。缺了就只能把初值降级成 `main` 里的 store，产物会变。
3. **`in_scope(...)` 是编译期谓词，不限于 `#if`**：它也出现在裸 `if` 里。判定与剪枝都在 sema
   （剪掉不成立的那一支），HIR 里**不出现** `in_scope` 调用，也不靠 `Const` 回传给 codegen 去区分
   （否则 `if 1` 这种真常量条件会被误剪，行为就变了）。

**六条不变量，每条一个 `assert`**：I1 每节点 `ty` 非空（除 Void 语句）I2 `Local`/`Global`/`Capture` 指向已解析声明
I3 `Call` 的 callee 一定已在 `funcs` 里（实例闭包已收敛）I4 无隐式转换（宽度/符号变化必经 `Cast`）
I5 每节点带 span I6 `funcs[].key` 唯一。

**接缝判据**：`hirdump` 把 HIR 打成前缀式一行一节点，可直接 diff；sema 单独喂 AST 即可产出它，不碰 LLVM。

## 2c. S2c 切割底数（`work/tmp/codegen-cut/`，HEAD=8af9d21）

- `ast_dep.tsv` 802 行（AST 引用逐处判决：改吃 HIR 297 / 迁 sema 288 / 删除 97 / 接口改造 88 / 拆 32）。
- `reasoning.tsv` 44 行（14 个推理函数的每个调用点，与 `grep -c` 逐函数对账一致）。
- `keep_list.md` 81 条（纯 IR 发射成员函数，按函数给理由）。

三个最高风险（S2c 前必须处理）：

1. **实例闭包 + 符号名**：现状是「发函数体时又发现新实例」的按需递归；名字带 `$<sig>`/`$c<n>`/`#L<lo>`，
   还有 64 上限。sema 必须先把闭包算完，并逐字复现这些名字。
2. **HIR 字段缺口**：见 §2b 补漏 1、2。
3. **隐式转换的粒度**：`convert` 今天自己按 `TyKind` 决定 `trunc/zext/sext/Raw`，窄常量加宽还走 `pattern_of`；
   只要与 sema 插 `Cast` 的粒度差一点（尤其 `Ptr`↔`Int` 的 `Raw` 与 `is_bool` 名义位），产物会**静默**变。

## 2d. 错误模型（冻结，落码 `sema/errors.hh`）

主设计 §3.4 要求三轴从**第一层**就带全。现在 `Result` 五字段里只有 `out[3]/out[4]` 放消息，责任与位置无处放，`catch(...)` 又全折成 `InternalError`。
定下来（新代码一律用，旧路径 S3 再迁）：

```cpp
namespace simplexc::sema {
enum class Responsibility { UserSource, Unimplemented, OurBug };
enum class Stage { Lex, Parse, Sema, Codegen, Assemble, Shell };
struct CompileError {
  Responsibility resp;
  Stage stage;
  Span span;           // line/col；0/0 = 无位置
  std::string detail;  // 具体是什么构造/为什么不行
};
}  // namespace simplexc::sema
```

约定：

- `Unsupported`（旧）与 `CompileError`（新）**不共存于同一个函数**；新写的 sema 代码只抛后者。
- 责任判定：sema/codegen 认不出的源码构造 = `Unimplemented`；同一类输入两次行为不一致、内部 assert 失败 = `OurBug`；
  语法/类型错误 = `UserSource`。
- 过 C 边界时**不许只剩类别名**（`catch(...)` → `InternalError` 是禁止的）；S3 做这一步。
- P1..P6 六条静默路径按 §4b 的 D1/D2 办：能不改行为的就不改行为，但必须**可查**。

## 3. 分步与提交边界

| 步 | 内容 | 判据 | 提交 |
| --- | --- | --- | --- |
| **S1** | 搬 `lower.cc` 的 31 个自由函数 + `module_build.cc` 到 `sema/`（不动类定义、不改一行逻辑） | `blob_baseline --verify` 98/98 一致 + check.py 全 PASS | 1 个 |
| **S1b** | 搬类型系统 + 常量折叠到 `sema/types.hh`；搬 19 个干净成员函数；`class Lower` 按语义侧/LLVM 侧拆成员 | 同上 | 1 个 |
| **S2a** | 新增 `sema/hir.hh` + HIR dump（`hirdump`，不进产品），不接线 | 编译过、check.py 全 PASS、dump 能出 98 份 | 1 个 |
| **S2b** | sema 生成 HIR：类型/常量/绑定/实例闭包（sema 侧完整） | 最小用例 + dump 抽样核对；不接线 | 1–2 个 |
| **S2c** | codegen 改吃 HIR（一次性切换），删 `static_type`/`cv`/`unify` 等推导 | 98/98 逐字节一致 + check.py 全 PASS | 1 个 |
| **S2d** | 拆 `V`/`Binding`/`Capture`/`FnState`：消 save/restore，递归改子上下文 | 同上 + `grep save_state` == 0 | 1 个 |
| **S3** | 错误三轴 + 消除 §3.4 的 6 条静默降级 | 失败用例表 + 回退率按三轴 | 1–2 个 |
| **S4** | §7.6：最小用例、assert、接缝；重跑 M5 判据重记账 | check.py 含新步骤 | 1–2 个 |

S1 与 S1b 是「纯搬」，产物必须逐字节相同；S2c 起也要求逐字节相同（纯语义保持），
唯一允许改变产物的是 S3（错误路径）与 S4 之后的优化。

## 4. 写者分区（并行度）

- 同一时刻：`sema/` 一个写者、`codegen/`（原 `lower/`）一个写者、`tools/` 一个写者。
- S2b（sema）与 S2c（codegen）可以在 `hir.hh` 冻结后**并行**，前提是 hir.hh 只由主程改。
- `docs/` 只由主程写；子 agent 的过程写 `docs/notes/<agent>-<topic>.md`。

## 4b. §3.4 勘误与两个决策（依据 `work/tmp/silent/paths.md`，HEAD=b5d90c9）

设计 §3.4 列的六条静默路径，两处引文不准、一处定性错。specs 只读，所以勘误记这里，S3 执行时按此办：

| 条 | 设计原文 | 实测 |
| --- | --- | --- |
| P5 | `parser.cc:292-311`「顶层未知行被解析成合法表达式」 | 引错位置：那是 `parse_block`。顶层未知行在声明扫描 `decls.cc:479-484` 就被丢进只写不读的 `top_unsupported`，**从未进解析器**；rc=0 照出 blob 是真的 |
| P6 | 「不可达 def 里的语法错不报」列为静默降级 | **定性错**：原版编译器同样不报（`bad.si` 死代码多一个 `)` → rc=0、8581 B）。判定点是 `module_build.cc:165` 的唯一闸门。反向：不可达 def 里的**词法**错照样拦（rc=1） |
| P1 | `parser.cc:344` | 兜底在 341-352，344 是中间行 |
| P4 | 「对 struct 字段只 catch 并继续」 | 位置对（3313-3329），但错在**位置**：它把错误记进 `record`（518-521）而不是抛 `Unsupported`，所以后续流程当它「已处理」 |

**D1（P6 怎么处理）：显式记录，不改行为。** 差分是唯一正确性判据（§7.1），「不可达 def 里不报」是与参考一致的保真行为，不是缺陷。
所以「禁止静默降级」= 不能**悄悄**，不等于必须变行为：记为 `responsibility=用户源码错 / stage=语法 / span 有` 的可查条目，行为照旧。

**D2（P2 的 notes）：先给 warnings 加消费者（sidecar + hook.log），不升级成错误。** 只有当某条 note 表示「我们跳过了某个语义」时才升成 `UnsupportedConstruct` —— 那才叫降级。

## 5. 已知风险

- S2c 一次性切换若产物变化，定位靠三件：`ir_text()` 逐函数 diff、单份语料 CLI 重编、git bisect 到块级。
- `ensure_def_fn`（75 行，换侧 10 次）是「发函数体时又发现新实例」的按需生成；分层后必须改成
  sema 先算闭包。它是 S2b 的最难点，先做它再谈其他。
- 传 `Ty` 的 `is_bool` 名义位不能丢（插值渲染 `true`/`false` vs `1`/`0`），否则 S2 会静默改行为。

## 4c. S3 工单底数（`work/tmp/errors-cut/`，HEAD=59a0297）

- 错误产生点 **252**（218 抛点 + 34 非异常通道：装配 12 / C 边界 6 / 引用出参 6 / decl_errors 2 / top_unsupported 1 / notes 2 / warnings 2 / Diagnostic 1 / GetLastError 2）。
- 捕获点 **50**（35 catch + 10 引用出参 + 5 非 catch 静默），其中 `catch (...)` 2 处、`GetLastError` 2 处、`errno` 0。
- **23 个位置必须全覆盖**（2 个 `catch(...)`、10 个引用出参、6 个 catch 内静默吞、5 个非 catch 静默），逐条清单在 `work/tmp/errors-cut/README.md` 第③节，脚本断言一个不缺。

**boundary 的答案：位置（span）这一轴现在完全过不去。** 返回值只有 5 个类别名；失败时三个出参不动；
detail 只进 `std::cerr`，而游戏是 GUI 进程、壳只重定向改写子进程的 stderr。"阶段"连定义都没有；
"责任"只剩类别名（`CompileFail`/`std::exception`/`catch(...)` 全折成 `InternalError`）。

三个坑与处置：

1. `frontend/ast.hh:35` 有个**零使用**的 `CompileError`（`ErrKind` + `message` + `Span`），字段与 §2d 不同。
   **决定：删掉这个死代码，统一用 `sema/errors.hh`**（与接口冻结版 §1 的字段一致）。不删的话谁先去 ast.hh 拿它，字段就对不上。
2. `hook.log` 的 `stage=ours|ours_timeout|ours_fault` 与 `CompileError::Stage` 同名不同义（实现模式 vs 编译阶段）。
   **决定：hook.log 那个字段改名**（如 `impl=`），否则"回退率按三轴统计"会串味。
3. `Unsupported` 只有行号，且 **10 处传 `0`**。三轴改完了，失败用例表照样分不出版。
   **决定：S3 逐个补真实 span**，不许留 0（「第 0 行」不会自己消失）。

## 2e. S2b-3 结果与 S2c 的三个前置决定（2026-10-24）

S2b-3 完成：19 个 case 全过（`analyze_selftest`），`sema` 侧不再有「未实例化的调用」。三处修正在现有实现里没有先例，记下来免得 S2c 以为是笔误：

- 同名重载的实例键要把**声明参数类型**折进去（`inst_sig()`），否则整个重载集被并成一个实例。
- 嵌套 `def` 用**词法键** `owner|名字@行`，不是「调用它的实例键」——按后者，自递归/互调会无界发实例（实测栈溢出、rc=127 空输出）。
- 嵌套实例同样有 64 上限，超了报「闭包实例数不收敛」。

**S2c 的三个前置决定：**

1. **Str 转义要暂时关掉**。sema 现在能解转义（U-L10 表），但 `lower` 仍拒绝反斜杠。若接线时一起放开，含反斜杠的语料会从「报错」变成「出产物」——这是新能力，不是回归，但会让「产物逐字节相同」这条判据失效、且分不清。做法：S2c 期间保持现状行为（拒绝），全绿后单独一步开启并单列基线差异。
2. **嵌套 def 的产物名归属**：现状是 `owner + "$" + name + "#L" + src_line`，其中 `owner` 取当前函数的**逻辑名**（不是实例键），`#L<行>` 兜住重名。S2c 派生符号名前必须先核对 `lower.cc` 的 `ensure_nested_fn` 取值处，别按「实例键」猜。
3. **`$` 参数（别名语义）HIR 仍无表示**，继续报未实现。S2c 不许为了「覆盖更多语料」去补这条——它是独立语义缺口，与分层无关。

目录改名（`lower/` → `codegen/`）单独一步做，不与内容切换混在一个提交里：改名会让 git 的 rename 检测吃掉 diff 可读性，混在一起就找不到「哪一行逻辑变了」。

## 2f. 内建与前缀函数的 HIR 表示（2026-10-24 冻结，供 S2b-4 与 S2c 并行）

起因：S2c 开工时实测 **`sema::analyze` 对 98 份语料 0 份成功**。第一道墙是顶层 `var` 的
`raw_cast(<Arr[U64]>, <常量地址>)`（常量求值不认 `raw_cast`），后面是内建、前缀函数、方法调用、
运行期全局初值。这四样在 98 份里每份都有，所以 S2c 不能等，也不能靠改判据绕过。

**`HBuiltin` 只许追加**（现有 `Load=0 / Store=1 / RawCast=2` 不动，改号等于悄悄改掉所有 dump 的含义）：

| 编号 | 名字 | 备注 |
| --- | --- | --- |
| 3 | `asr` | 算术右移；有符号性影响语义 |
| 4 | `clz` | `llvm.ctlz.i64`；返回类型照现状 |
| 5 | `ror` | 循环右移 |
| 6 | `min` | 取窄侧类型规则照 `builtin_minmax` |
| 7 | `max` | 同上 |
| 8 | `array` | `<T>, n` 每次调用点一枚私有全局 |
| 9 | `random` | 递推是**顺序赋值**（`x^=x<<12; x^=x>>25; x^=x>>27; x*=C`），别展平 |

**前缀函数不是内建**：`get_time` / `sleep` / `memory_clear` / `memory_copy` / `thread_exit` 是源码里定义在
前缀中的普通 `def`，语义上走 `Call{FnId}`。但 codegen 有优化过的 IR 实现（`body_*`），所以：

> `HirFunc` 加 `pre_body { None, GetTime, Sleep, MemoryClear, MemoryCopy, ThreadExit }`。
> sema 判定「这个实例的 def 来自前缀且名字是这五个之一」时设置；codegen 见到非 `None` **不编译 body**，
> 改走对应 `body_*`，产物符号名与现状一致。

**顶层 var 的运行期初始化**：语料形态是 `raw_cast(<Arr[U64]>, <常量地址>)`——它**是**编译期常量，
缺的只是常量求值不认识 `raw_cast`。先把 `cv` 扩到认 `raw_cast(<T>, 常量)`，**不要**为此引入
「运行期初始化」这个新概念（那会改 `HirGlobal` 的语义，波及 codegen 的 `make_globals`）。

**方法糖**（`.len/.push/.pop/.contains/.next/.min/.size/.write/.array/.cstring` 与时间单位
`.ms/.ns/.us/.sec/.day/.hour/.min`）：糖消解归 sema，照 `lower.cc` 的 `call_method` 逐条对应；
HIR 里只留消解后的结果，不留方法名。

## 2g. sema 语料覆盖率实测（2026-10-24，`work/tmp/sema-survey/`）

**98 份语料：OK 0 / FAIL 98 / 崩溃 0。** 基线里出产物的那 75 份：sema 走通 **0 份**（等式 75 = 0 + 75）。
- FAIL 分类：98 份全部倒在第一道墙 —— 顶层 `var` 的 `raw_cast(<Arr[U64]>, <常量地址>)`（常量求值不认 `raw_cast`）。
- 剥掉第一道墙后（`peel1.tsv`）暴露的后续缺口：内建 7 个、前缀函数 5 个（各有 98 份命中）、方法糖、运行期全局初值。
- 复现：`work/tmp/sema-survey/run_survey.sh`；判据是自己编到 `work/tmp/`，不碰 `build/`。

后果（记下来免得再犯同样的判断错）：

1. **「语言形态覆盖率」不是「语料覆盖率」**。19 个最小用例全过，等于 0 份语料能过。
2. **S2c 不能靠 98/98 自证**。它的中期判据换成「双跑对照」（同一份 `.si` 走 AST 路径与走 HIR 路径，blob 逐字节相同），
   样本是那 19 个能产出 HIR 的 case。
3. **产品入口不跟着切**。HIR 入口先只由 `work/tmp/s2c/` 的双跑工具调用，产品路径仍是 `ModuleAst`
   —— 于是 `check.py` 与 98/98 在整个 S2c 期间保持绿，护栏不丢。入口切换等 sema 覆盖达标后单独做，那时是个位数行的改动。

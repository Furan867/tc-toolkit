# analyze 的最小用例

每个 `.si` 只测一件事，同名的 `.dump` 是 `build/analyze_selftest.exe --dump-only <case>.si`
的输出（**重定向生成，不手抄**），`analyze_selftest` 拿它逐字节比 + 对同一棵树跑
`check_invariants`。生成之后要对着规格（`docs/notes/2026-10-24-refactor-plan.md` §2b）与
`lower.cc` 逐行核：值、类型、节点顺序、span。

| 用例 | 只测这一件事 |
| --- | --- |
| `01_fold` | 常量折叠：`+` 取窄侧（`arith_ty`）、截断在运算结果上、`>>` 是 lshr |
| `02_globals` | 三种名字来源：顶层 `const #N`、带 `init_bits` 的全局、局部 |
| `03_casts` | 隐式转换显式化成 `Cast`（U8 → U64 出 `zext`，同类型不插） |
| `04_bool` | `Bool` 的名义位（`Bool 2` 按 1 位截断、dump 打 `Bool` 而不是 `U1`） |
| `05_stmts` | 语句形态：裸块 / 赋值 / 复合赋值 / `let` 常量绑定（零条指令）/ 带标签 `break` / 裸 `return` |
| `06_if` | 运行期 `if`：`If` 三个位、`elif` 压成嵌套（里层 `If` 装在一个合成 `Block` 里）；`if 1` **不**被剪 |
| `07_in_scope` | `in_scope(…)` 的剪枝（裸 `if` 与 `#if` 一视同仁）：假的块整块丢、真的块成一条 `Block`；三档判定各一个名字 |
| `08_switch` | `switch`：独立条件（无 fallthrough）、`default` 排到最后、`break` 穿到外层循环、`Str` 主体按内容派发 |
| `09_array` | 数组字面量（静态片，类型取第一个元素）/ `Arr[T] []` 空数组 / 下标是位置节点 |
| `10_struct` | struct 布局（声明序、字节偏移、无 padding）+ `Hir.structs[]` + 复合字面量（按名字匹配、按声明序摆）+ `Member` 的序号/偏移 |
| `11_enum` | enum 底层宽度（U-L5：≤256 → 8 位、257…65535 → 16 位）+ 成员是序号常量 |
| `12_str` | `Str` 字面量进串池 + `Str == Str` 不折叠（`op=streq`/`strne`） |
| `13_template` | 模板插值：文字段/表达式交替（kid 个数 = 2×插值数 + 1） |
| `14_prefix` | 前缀全局 `global_seed` 按需进 `Hir.globals`（下标在顶层全局之后、初值 = 1） |
| `15_call` | 顶层 def 调用：同名重载按实参类型挑（`pick$bool` / `pick$str` 两个实例）、自递归、互调（前向引用也能挑到） |
| `16_generic` | 泛型单态化：同一个 def 的 `@Type`/`@Any`/`@Size` 不同绑定 = 不同实例（键里的 `$@u8#v` 等）；类型实参的两种写法（值形带 `#v`、`<U16>` 形式不占运行期槽）；`@Type` 当返回类型 |
| `17_closure` | 嵌套 def 与捕获：`capture[i].index` = 首次遇到顺序、`cap_index` 只给顶层 def（嵌套恒 -1）、捕获面不同 = 两个实例（`$c0`/`$c1`）、递归/互调按词法键收敛到同一个实例 |
| `18_strcat` | `Str` 拼接（`Binary{Add, ty=Str}`，开缓冲是 codegen 的事）+ U-L10 的转义表（`\t` → 0x08、`\b`/`\x42` 原样留反斜杠），模板文字段走同一处 |
| `19_cap_runaway` | **报错用例**：同一个顶层 def 的第 65 个捕获签名 → `CompileError{Unimplemented, Sema}`（不是死循环、不是栈溢出），断言文本在同名 `.err` |

两份 golden 的形状：

- `<case>.dump`：`analyze` 成功时的 dump，逐字比 + 六条不变量（默认）。
- `<case>.err`：**该报错**的用例，`analyze` 必须抛异常且 `what()` 与 golden 逐字相同；这类用例没有
  dump、也不跑不变量（没有树）。一份 `.si` 只能有 `.dump` 与 `.err` 之一。

每个用例的根都是语料里唯一的形态 `run_sim: while true { … }`：`decls.cc` 的 `match_label`
要求 `名字:` 同一行跟 `while|for|loop`，别的写法整行会被当成未知行（根块不存在）。

重新生成（仓库根，**别手抄**）：

```bash
python simplexc/tools/build.py analyze_selftest
for f in simplexc/sema/testdata/cases/*.si; do
  build/analyze_selftest.exe --dump-only "$f" > "${f%.si}.dump"
done
```

报错用例的 golden 用 `--err-only`（没报错时它返回 1，不会静默生成空文件）：

```bash
build/analyze_selftest.exe --err-only simplexc/sema/testdata/cases/19_cap_runaway.si \
    > simplexc/sema/testdata/cases/19_cap_runaway.err
```

`.dump` 里是 CRLF（Windows 下 stdout 是文本模式），而 dump 只出 LF；`analyze_selftest.cc`
读进来时把 CRLF 折成 LF，所以 `core.autocrlf=true` 检出成什么样都不影响判据。

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

每个用例的根都是语料里唯一的形态 `run_sim: while true { … }`：`decls.cc` 的 `match_label`
要求 `名字:` 同一行跟 `while|for|loop`，别的写法整行会被当成未知行（根块不存在）。

重新生成（仓库根，**别手抄**）：

```bash
python simplexc/tools/build.py analyze_selftest
for f in simplexc/sema/testdata/cases/*.si; do
  build/analyze_selftest.exe --dump-only "$f" > "${f%.si}.dump"
done
```

`.dump` 里是 CRLF（Windows 下 stdout 是文本模式），而 dump 只出 LF；`analyze_selftest.cc`
读进来时把 CRLF 折成 LF，所以 `core.autocrlf=true` 检出成什么样都不影响判据。

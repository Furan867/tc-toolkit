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

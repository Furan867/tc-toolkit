# 路线 A：前缀参与编译 —— 前端构造缺口逐类的**改动量实账**

日期：2026-10-18 · 只改 `simplexc/frontend/**`（lowering 未动）· 器材 `work/tmp/routea/**`
基线：工作树 `f6659f1`+未提交（`lower/**`、`docs/notes/2026-10-18-m5-coverage.md` 是别的写者，我不碰）。

---

## 0. 结论先行

1. **4 类缺口里，前端只欠 3 类，而且都很便宜**：① `$` 传引用（3 文件 / 约 12 行）· ③ `[T] expr` + `.'引号成员名'`（4 处 / 约 40 行）
   · ④ 多行 `Enum[`（1 文件 / 3 处 / 约 15 行）。**② 编译期类型参数不是前端缺口** —— 报错原文出自
   `lower/lower.cc:854,878`（`Unsupported("编译期类型参数 …")`），前端一路解析通过。
2. **补完 ①②③④ 后，前缀 127 个 def 里只剩 5 个解析不过，全是 inline asm**（第 101/145/188/202/567 行）；
   而 asm 是 brief 里点名要停的 lowering 类缺口 → **路线 A 的前端在「本轮到不了的地方」收工**。
3. **asm 不是只挡前缀**：`memory_clear`（纯 asm 体）被 **98/98** 份语料调用，`memory_copy`（同为 asm）**27 份**。
   所以即使前端补完，路线 A 的整链仍卡在同一面墙上 —— 这面墙的账要算在 lowering 头上（是否可换成内建/内联）。
4. 前端改动**没有**碰到语料侧：183 个单元的解析通过率逐条对比**只有 `campaign/sort/test.si` 前进**
   （`$` 那个 def 现在过了，剩下的是它自己既有的 `set_error((a, b))` 问题）。`check.py` **5 PASS**。

---

## 1. 器材：拼接驱动 `work/tmp/routea/drive.py`

游戏喂给编译器的 = **固定前缀 + 芯片源码两份拼接**（拼法照 `work/hook/hook_src_001.si` 的结构：
语料自带的「游戏内存地址声明 + 类型」在最前，前缀跟在前面）。驱动做三件事，各自落 `work/tmp/routea/<名字>.tsv`：

| 子命令 | 跑什么 | 口径 |
| --- | --- | --- |
| `parse` | `build/ast_dump.exe --src <语料> --all --json` | 单元 = `corpus/**/*.si` 按 sha256 去重（183 个：src 98 + rv32im + campaign 去重） |
| `concat` | 同上，源 = 前缀 + 语料 | 同上 |
| `cli` | `build/simplexc_cli.exe --src 拼接源 --out <bin>` | 整条链（前端 + lowering + LLVM） |

**单元级判据只认「解析失败」**（`error_msg` 非空）：`ast_dump` 的 `ok:false` 还包括「有未覆盖行」，
把覆盖度提醒算成失败会把 8 个坏 def 看成 8 个（其中 3 个是提醒）。单元级报错取**前缀块里行号最小的那个 def**。

```text
python work/tmp/routea/drive.py parse      # 语料本体（不含前缀）
python work/tmp/routea/drive.py concat     # 前缀 + 每份语料
python work/tmp/routea/drive.py cli --limit 4
```

---

## 2. 逐类的实测（改动量 + 通过率 + 报错原文）

### ① `$` 传引用形参（+ `$x` 取引用）—— 3 文件 / 12 行

`$` 只是 sigil：形参写 `$a: Ptr`，用的时候写 `$a`（`$global_seed.next()`）。sigil 按 `#` 的老规矩
**并进名字**（`Param.name = "$a"` / `IdentExpr.name = "$a"`），不新增 AST 字段 —— 新增字段会让
`--json`/文本树多一个冻结原型没有的键，整棵树的 deep-equal 就比不了了。

| 文件 | 改动 |
| --- | --- |
| `frontend/lexer.hh`/`lexer.cc` | `$name` → 新 token 种类 `dollar`（与 `hash`/`at` 同一分支，+3 行）；`describe()` 加一行 |
| `frontend/parser.cc` | `parse_params` 收 `dollar`（+2 行）；`parse_primary` 收 `dollar` → `IdentExpr{"$"+name}`（+7 行） |

通过率变化（183 单元）：
- `parse`：**183/183 → 183/183**（`campaign/sort/test.si` 的 `$` 定义现在过了；它的下一个错从第 48 行退到 42 行，
  剩下的是既有问题，见 §4）
- `concat`：**0/183 → 0/183**，但**报错位置从第 172 行推到第 101 行** —— 这才是这一步的真实收获。

报错原文（拼接驱动，改前 → 改后）：

```text
错误：第 172 行 第 12 列：第 172 行：不认识的字符 '$'          # 改前：`binary += ($a: Ptr, b: AnyInt) None {`
失败 第 101 行：语句位置出现没有副作用的表达式（CastExpr）——是不是把 '=' 写成了 '=='？   # 改后：print_using_stack 的 asm aarch64
```

### ② `@Any` / `@Type` / `@Size` —— **前端 0 行**

判据：`grep -rn 编译期类型参数 simplexc/` → 只有 `lower/lower.cc:854,878` 两处 `Unsupported`。
前端侧 `@T` 早就是一等 token（`type_ref_after` 里 `tr->at`），`quick_sort($arr: Arr[@Any])`、
`dot push($array: Arr[@Type], …)`、`hex(num: @Size)` 这些 def **都不在失败列表里**。
所以 ② 的成本**整条在 lowering**（泛型单态化 / `size(@Size)` 求值），不在本轮写区。

### ③ `[T] expr` + `extern` 命名空间 + `.'引号成员名'` —— 4 处 / 约 40 行

| 子构造 | 改动 | 说明 |
| --- | --- | --- |
| `.'引号成员名'` | `parse_postfix` 成员名位置接受 `string` token（+3 行） | `kernel32.'ExitProcess'(code)`；引号只是拼写容器，成员名照原样存 |
| `[T] expr` | `parser.hh` +2 行；`parser.cc` 新增 `bracket_cast_ahead()`/`parse_bracket_cast()`（约 28 行） | 与数组字面量共用 `[`，靠「类型名 + `]` + 后面还跟着表达式」区分；`[U64] a` 与 `Arr[U64] a` 是同一个概念，**存成同一个拼写**（`name="Arr"`），免得 lowering 认两种写法 |
| `Str [input]` | `type_ref_after`：`[` 要**紧邻**才算元素类型（+1 行条件） | `return Str [input]` 是「`Str` 转换 + 单元素数组字面量」，而原型/旧实现把 `[input]` 吃成了元素类型 |

`extern windows_x64 kernel32` 本身**解析没问题**（它是顶层 `extern` 声明，decls 扫得到，报错的是它的调用点）。

通过率变化：`concat` 里前缀的坏 def **16 → 8**（①后 16 个 → ③后 8 个）。逐个对照（前缀 + `src/006.si`）：

```text
①后（个数 16）                        ③④后
101 print_using_stack  asm           101 asm          ← 无变化（asm）
145 breakpoint         asm           145 asm
188 memory_copy        asm           188 asm
202 memory_clear       asm           202 asm
254 exit               .'name'       过了
259 memory_reserve     .'name'       过了
269 memory_commit      .'name'       过了
280 memory_free        .'name'       过了
291 thread_exit        .'name'       过了
542 output             .'name'       过了
549 sleep              .'name'       过了
555 get_time           .'name'       过了
567 get_cycle_count    asm           567 asm
656 big_int_add        [T] expr      过了
770 big_int_xor        [T] expr      过了
1274 str               Str [input]   过了
```

### ④ 多行 `Enum[` —— 1 文件 / 3 处 / 约 15 行（`frontend/decls.cc`）

`decls.cc` 只按**花括号**配平，`type Char Enum[` 换行后声明体的 `hi` 停在 `[` 那一行 → lowering 的
`module_build.cc:71` 拿不到 `]`，报 `枚举声明看不懂`。改法：扫描时**同时数 `[`**，`bracket > 0` 时
声明不收口。顺带消掉 6 条假「未支持的顶层构造」（枚举成员行）。

整链（`simplexc_cli.exe`）的拦路面因此从第 17 行推到第 202 行：

```text
编译失败：第 17 行：枚举声明看不懂：type Char Enum[                            # 改前
语法错误：第 202 行 第 0 列：语句位置出现没有副作用的表达式（CastExpr）——…      # 改后（memory_clear 的 asm x86_64）
```

---

## 3. 现在的拦路面：inline asm（**交主程**，不在本轮写区）

前缀 5 个 def 的体里只有 asm：`print_using_stack`(101, aarch64) · `breakpoint`(145, x86_64+aarch64) ·
`memory_copy`(188) · `memory_clear`(202) · `get_cycle_count`(567, 含 `mov $result, rax`)。

- **前端**（可做，但不是本轮范围）：`asm <target> { …raw… }` 需要一个不透明语句节点 + 花括号配平跳读，
  `ast.hh`/`parser.hh`/`parser.cc`/`ast_dump.cc` 四处，估 **40–60 行**（抄 `parse_block`/`BlockStmt` 的形状）。
- **lowering**（真正的成本）：asm 的操作数是**源码层名字**（`mov rsi, source`、`mov $result, rax`）——
  不是汇编，是一台小汇编器；且 `memory_clear` 被 98/98 份语料调用。要么真做这台小汇编器，
  要么把这两个内存原语换成内建（`rep stosb/movsb` 语义 = `memset`/`memcpy`，我们 lowering 里已有等价的循环可复用）。

## 4. 与路线 A 无关但挡在 `parse` 里的既有缺口（52 个单元）

`campaign/*/test.si` 里 `set_error((31337_70376380014778, \`…\`))`：形参只有一个
（`src/001.si:189 def set_error(input: Str)`），但调用实参写成 `(a, b)` 的括号组 —— 前端报
`这里要 ')'（配对第 N 行的 '('），看到 ','`。**改前改后同一个数**（52），本轮没碰。这 52 个都在
`campaign/`（关卡测试源），`src/` 98 份一份都不在这个缺口上。

## 5. 复现命令

```bash
python simplexc/tools/build.py ast_dump simplexc_cli
python work/tmp/routea/drive.py parse       # 131 PASS / 52 FAIL（改前改后逐条相同，除 sort 前进）
python work/tmp/routea/drive.py concat      # 0 PASS / 183 FAIL，全部停在第 101 行 asm
python work/tmp/routea/drive.py cli --limit 4
python simplexc/tools/check.py              # 5 PASS
```

## 遗留风险

1. `[T] expr` 我按「`Arr[U64] a` 的另一种拼写」存进 AST（`name="Arr"`）—— **这是解释**，
   依据是前缀里 `[U64] a` 全部当数组用（下标、喂 `grow`）；没有单独的参考实现对照过。
2. `type_ref_after` 的「`[` 要紧邻」是新规则（原型无条件吃 `[`）：前缀与 98 份语料里
   没有一处「类型名 + 空白 + `[` 元素类型」，所以判据上安全；但语料之外未知。
3. 拼接驱动只量了 `corpus/**` 183 个单元；`campaign` 里与 `src/` 逐字节相同的副本已去重，
   剩下的 campaign 单元在 §4 那个既有缺口上。
4. `work/tmp/routea/**` 不进 git（是证据，不是产物）。

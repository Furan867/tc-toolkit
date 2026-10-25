<!-- 整体重写，禁止追加。≤80 行。
     一次性结论 → notes/；被复用过的结论 → facts/。具体事实不在这里复制。 -->
# 状态

在追：**M5**。目标是把 `compile.dll` 换成自研实现（Simplex → LLVM IR → 装配成游戏可 JIT 的 blob）。
正确性判据：同一份 `.si`，我们的产物与参考产物**逐 tick 状态等价**。

- 接口权威 = `docs/specs/2026-10-15-simplexc-interfaces.md`（冻结版 1.4）；主设计 = `…-simplex-llvm-compiler-design.md`。
- 代码在 `reversing/simplexc/`；LLVM 固定 23.1.3。语义档案 `docs/facts/semantics.md`。

## M5 stdlib 全量兼容：编译层已达成（2026-10-25）

`corpus/stdlib_prefix.si`（1503 行 / 27711 B，从 `compile_real.dll` 偏移 `0x119F48` 原样提取，
拼在每份语料之前一起编译）现在**能编过**。为它补的东西：

| 关口 | 补了什么 |
| --- | --- |
| `type X <拼写>` | 类型别名（右手可 `Arr[Char]` / `Arr[Arr[U8]]`）；别名查表放在内建名之后 —— `Str` 必须仍是 `TyKind::Str` 而不是 `Arr[Char]` |
| `call_conv` / `extern` | 元数据声明：认形状、留痕、不产代码 |
| `array(<T>, n)` | 长度可为运行期值（前缀的 `array(<U64>, a.len())`） |
| `asm x86_64 { … }` | 白名单 7 助记符（`mov idiv shl or rdtsc rep movsb rep stosb`）+ `$槽` 访存；12/13 条与参考逐字节对齐（余两条是寄存器分配器差异）；非 x86_64 块整块丢弃且**不验证内容** |
| `kernel32.'Name'(…)` | 编成外部调用（`movabs+call` 桩 + stubs sidecar）；名字走**显式表**（7 条），表外报未支持而不是猜 |
| 重载择优 | `pick_def` 先非泛型候选、再泛型候选（参考按实参具体度挑，与声明序无关） |
| `$` 可变引用 | **真别名**（指针形参，不拷贝+写回）；`$global_seed` 是顶层 var 的可变引用 |
| `dot` 方法分派 | 按接收者静态类型查单元；`.len()` / `.ms()` 内建仍优先（否则前缀的 `dot len` 体会无限递归） |
| `static_type` | Ident 档补「程序根体的 `const #`」（`root_consts`）；Call 档补 `random` / `ror` |

判据陷阱记在 `docs/pitfalls.md`：`scan()` 的「未支持」判定在花括号计数之后，所以裸头开 `{` 的行
（`binary +` / `dot` / `index_read`）**永远不报错也不进声明表** —— 「错误行号变大」不等于支持了。

## 判据（口径 2026-10-25 修订）

verdict **带 `--src`**，只比 `[game_lo, game_hi)`。旧口径比全内存，会把**产物自己的代码区字节**
与**栈帧布局**也算进判据（这两样每份都不同），于是「产物变一点就判 diff」。修 `--src` 的提交是 `36470ce`。

98 份 × 100 tick：

| 模式 | equal | diff | cli_fail | host_fail |
| --- | --- | --- | --- | --- |
| **加前缀**（真编译 stdlib） | **91** | 5 | 0 | 2 |
| 不加前缀（靠 5 个硬编码兜住） | 77 | 3 | 17 | 1 |

加前缀明显更好 —— `compile()` 内部本来就自动加前缀，所以 `corpus/ref/*.bin` **本身就是含前缀的参考产物**。

`check.py` 6 PASS；产物基线 81 份出产物 / 17 份报错。

## 未解

- **069 / 080 跑不起来**：都指向 `0x40800000`（`kStackBase + kStackSize`，栈区上界外一页）。
  069 是 LLVM 内联的 memcpy 向量循环读越界（`rsp` 只在栈顶下 16 KB，不是栈溢出）；080 是重复映射。
- **5 份真差异**：028(124 B) / 034(198) / 084(30) / 096(30) / 098(29)。
  - **028 的根因已查明**（`430c46c`）：**与 stdlib 开关无关**。参考对 `str` 按**静态类型分派**
    （实测 `str(false)` → `"false"`、`str(U3 0)` → `"0"`），而 `pick_def` 在 7 个 `def str` 里只分
    「泛型 / 非泛型」两档、档内按声明序，于是 `str(U3 0)` 挑成了 `str(Bool)`。
  - 096/098 是随机流错位，差 34 次抽取（`docs/bugs/2026-10-25-random-stream-offset.md`）。
  - 034 / 084 未查。
- `thread_exit.1` 符号碰撞（`docs/bugs/`）：隐式收尾与前缀的 `def thread_exit` 同名。
- runtime 整块链接：不用 `array()` 的语料也带 `tc_array_alloc` 的 200 B + 一个 `__imp_VirtualAlloc` 槽。

## 环境

- `build/` 是共享的：多写者按目录切分，同一时刻只让一个写者跑判据入口。
- `python simplexc/tools/build.py` 默认目标**不含** `simplexc_cli` / `difftrace`，改了编译器要另跑。
- 判据入口：`check.py`（6 步 52 s）· `m5/batch.py`（`--with-prefix` 跑 stdlib 全量，
  `--jobs 12` 下 98 份约 5 s）· `difftrace`（run / diff / where）。
- `tc/.clangd`（不进 git，`tc/.gitignore` 首行是 `/*`）给 clangd 补 `-I`；不配的话
  `difftrace/` 里 `#include <unicorn/unicorn.h>` 会级联出 21 个假错（真构建 0 warning 0 error）。

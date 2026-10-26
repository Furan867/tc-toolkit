<!-- 整体重写，禁止追加。≤80 行。
     一次性结论 → notes/；被复用过的结论 → facts/。具体事实不在这里复制。 -->
# 状态

在追：**M5**。目标是把 `compile.dll` 换成自研实现（Simplex → LLVM IR → 装配成游戏可 JIT 的 blob）。
正确性判据：同一份 `.si`，我们的产物与参考产物**逐 tick 状态等价**。

- 接口权威 = `docs/specs/2026-10-15-simplexc-interfaces.md`（冻结版 1.4）；主设计 = `…-simplex-llvm-compiler-design.md`。
- 代码在 `reversing/simplexc/`；LLVM 固定 23.1.3。语义档案 `docs/facts/semantics.md`。

## M5 stdlib 全量兼容：编译层已达成（2026-10-25）

`corpus/stdlib_prefix.si`（1503 行 / 27711 B，从 `compile_real.dll` 偏移 `0x119F48` 原样提取）现在能编过。为它补的：

| 关口 | 补了什么 |
| --- | --- |
| `type X <拼写>` | 类型别名（右手可 `Arr[Char]` / `Arr[Arr[U8]]`）；别名查表放在内建名之后 —— `Str` 必须仍是 `TyKind::Str` |
| `call_conv` / `extern` | 元数据声明：认形状、留痕、不产代码 |
| `array(<T>, n)` | 长度可为运行期值 |
| `asm x86_64 { … }` | 白名单 7 助记符 + `$槽` 访存；非 x86_64 块整块丢弃且不验证内容 |
| `kernel32.'Name'(…)` | 外部调用桩 + stubs sidecar；名字走显式表 |
| 重载择优 | `pick_def` 先非泛型候选、再泛型候选 |
| `$` 可变引用 | 真别名（指针形参，不拷贝+写回） |
| `dot` 方法分派 | 按接收者静态类型查单元；`.len()` / `.ms()` 内建优先 |
| `static_type` | Ident 档补「程序根体的 `const #`」；Call 档补 `random` / `ror` |

判据陷阱记在 `docs/pitfalls.md`：`scan()` 的「未支持」判定在花括号计数之后，裸头开 `{` 的行永远不报错也不进声明表。

## 判据（2026-10-26 实测）

`check.py` 6 PASS（54.8 s）；`batch.py --with-prefix --jobs 12 --ticks 100` 98 份 **38.6 s**：

| 模式 | equal | diff | cli_fail | host_fail |
| --- | --- | --- | --- | --- |
| 加前缀（真编译 stdlib） | 95 | 3 | 0 | 0 |
| 不加前缀 | 95 | 3 | 0 | 0 |

3 份 diff 全部定性为**参考侧不可复现**（参考产物拿未初始化的栈值当随机种子），我们自己的语义差异 **0 份**：

- 084（`campaign/overture_1_registers`）→ `docs/bugs/2026-10-26-084-ref-uninit-stack-seed.md`
- 096 / 098（`campaign/overture_2_alu` / `campaign/overture_3_immediates`）→ `docs/bugs/2026-10-25-random-stream-offset.md`

⚠️ **分母口径待定**：这三份是否从 M5 分母剔除（→ 95/95 达标）还没定，别按「98/98」或「95/98 达标」对外说。

## 未解

- **031-032「逻辑试炼」的「游戏内崩溃」**：原结论（产物缺 20 个接口函数）已被推翻 —— 参考同样丢弃未调用的函数
  （删掉那 31 个 def，参考产物只差 4 字节），我们的产物根体完整、离线 100 tick 无异常。
  现在缺的是现场重新取证，见 `docs/bugs/2026-10-26-031-unreproducible-crash.md`。
- **游戏内「连续编译两次拿到的 `.si` 不同」未查**（031 vs 032：`get_gate_score` 14276 vs 722、组件 51 vs 128）。
  需要游戏内实验，不是 `difftrace` 注出来的。
- `thread_exit.1` 符号碰撞（`docs/bugs/2026-10-25-thread-exit-collision.md`）。
- runtime 整块链接：不用 `array()` 的语料也带 `tc_array_alloc` 的 200 B + 一个 `__imp_VirtualAlloc` 槽。

## 环境

- `build/` 是共享的：多写者按目录切分，同一时刻只让一个写者跑判据入口。
- `python simplexc/tools/build.py` 默认目标**不含** `simplexc_cli` / `difftrace`，改了编译器要另跑。
- 判据入口：`check.py`（6 步 55 s）· `m5/batch.py`（`--with-prefix` 跑 stdlib 全量，`--jobs 12` 下 98 份约 39 s）· `difftrace`（run / diff / where）。
- `tc/.clangd`（不进 git，`tc/.gitignore` 首行是 `/*`）给 clangd 补 `-I`；不配的话 `difftrace/` 里 `#include <unicorn/unicorn.h>` 会级联出 21 个假错。

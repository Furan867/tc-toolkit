# lower/ 零散缺口第三轮：Str 内容相等 + switch 主体 Str（29 份 CompileFail 归零）

口径变化：**31 可装配 / 20 全等 / 67 cli_fail** → **57 可装配 / 20 全等 / 41 cli_fail**（+26 份）。
器材：`work/tmp/lowermisc/`（最小复现、`001.o`/`001b.ll`、lint 配置）。

## 1. 做的唯一一件事：`Str` 按内容比

`s_SwitchStmt`（`lower.cc:1034`）对 `Str` 主体取 `subj.ll`（`ptr`），case 值走
`const_int(pat_const(...))`（`i64 0`）→ IR 校验 `icmp eq ptr, i64`。这不是「缺个类型转换」：

- 语料拿 `switch id` 派发 UI 元素，而调用点传的是**运行期插值出来的名字**
  （028:820：``ui_set_text(`table[5][{cycle + 1}]`, …)`` 那类）。按指针比永远命中不了 —— 参考按**内容**比。
- 同时 `Str == Str` 此前是 `icmp ptr` 的**静默错码**路径（`streq.si` 能编过），一并归到内容比。

实现（`lower.cc`）：

| 件 | 形状 |
| --- | --- |
| `__simplexc_streq(ptr a, ptr b, u64 n) -> 1/0` | 逐字节循环 + 不等即返回，仿 `ensure_copy_fn` 的独立 internal 函数（**不引外部符号**） |
| `str_compare(op, a, b)` | 长度（`len_of` = `p[-8]>>16`）不等就 `n = 0`（否则逐字节会读越过短的那条）；`==`/`!=` 之外报 Unsupported |
| `s_SwitchStmt` | 主体是 `Str` 时每个 case 用 `str_compare("==", …)` 出条件；case 值必须是 `Str` 常量 |

长度是必需的那一半：只比字节到 NUL 会让**插值缓冲**（不保证 NUL 终止）读进缓冲里的垃圾。

## 2. 判据

```text
python simplexc/tools/m5/coverage.py   → 可装配 31 → 57 · 全等 20 → 20（逐份 id 不变）· cli_fail 67 → 41
python simplexc/tools/check.py         → 5 PASS / 0 FAIL / 0 SKIP
ast-grep（pi-lens 298 条规则，入口见 2026-10-18-ir-build-lint.md）→ lower.cc/lower.hh 命中 0
```

全等集合逐份核对过（002 004 005 006 020 026 030 032 036 038 040 058 067 071 120 141 143 176 177 180），
没有「掉一份补一份」。

## 3. 修完后**新露出来**的三类（都不是这次 lowering 引入的，是以前过不了 IR 校验、代码根本没跑到）

| 类 | 份数 | 语料 | 现场 |
| --- | --- | --- | --- |
| HostRc：产物在宿主里 AV（**写**） | 9 | 001 047 048 049 050 051 053 054 124 | 见 §3.1 |
| Other：外部符号解析失败 `kernel32!memcpy` | 3 | 086 088 090 | `.ll` 里**没有** `memcpy`（grep 0 条）→ 是 IR 之后的 pass 把某个拷贝循环认成了 `memcpy` |
| 段哈希 / extern 差分 | +8 / +9 | 042 063 065 … | 之前是 CompileFail，差分根本没机会算 |

### 3.1 AV 的现场（有证据，根因未定）

```text
python simplexc/tools/host/host.py --src simplexc/corpus/src/001.si --bin work/tmp/lowermisc/001b.bin \
  --entry 0xB970 --ticks 3 --seed-reset-data seq --out work/tmp/lowermisc/001.ours.jsonl
[host] 异常 code=0xC0000005 addr=0x217666C9F2B（产物 +0x9F2B） params=[1, 856424841216]
```

- `params[0]=1` → 是**写**违例；`params[1]=856424841216=0xC766E60000` 是写的目标，**等于入口第二个参数的值**（同一次 coverage 跑里是 408699666432，两次跑目标不同 = 目标是被传进来的那个值）。
- 故障指令：`objdump -d --start-address=0x9F10 --stop-address=0x9F40 work/tmp/lowermisc/001.o`
  → `.text+0x9F2B` 是 `movups %xmm0,-0x10(%rbx,%r12,1)`，即 `__simplexc_copy` 被内联并**向量化成 32 字节块**后的那条 store；它在 `mode_run$get_input`（`objdump -t`：0x9540 起）里。
- **不是** `__simplexc_streq`（它只读；且它在 001 里被内联，符号不在 `.text` 里）。最像的形状是
  `template_expr` 的「有 `Str` 段 → 动态 alloca」：尺寸 `fixed + len_of(段)` 若拿到垃圾长度，
  `rsp` 直接跳到那个值上，随后拷贝写进去就 AV。**没验**，别当结论用。

## 4. 遗留

- 闭包（根体 `var` 被嵌套 def 引用）不是 2 份而是 **~30 份**：`counter` 12 · `test` 4 · `instructions` 3 ·
  `input` 2 · `memory` 2 · `count`/`input_index`/`last_mem`/`over`/`seen`/`size`/`was_read`/`x_pos` 各 1。
  逐份查过：每份都在**缩进 4** 的根体里声明（如 `147:805 var counter`），在嵌套 `def` 里引用（`147:1044`）。
  这条要改函数 ABI → 按停止规则报回去了。
- 前缀函数 `str` 3 份（028/034/080）· `bin` 1（131）· `hex` 1（122）：定义在
  `corpus/stdlib_prefix.si:1110-1234`，只用 `array(<U8>, n)` + 十进制/十六进制拆分，可在 lowering 里当内建实现
  （渲染语义与插值的 `render_part` 同一套）；`str` 三个重载按实参类型分派（`Str` 恒等 / `Bool` → true|false / 无符号→十进制）。
- `编译期类型参数 Size` 2 份（100/102）是**前端**的缺口，不在本轮写区。

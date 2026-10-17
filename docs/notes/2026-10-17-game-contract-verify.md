# 2026-10-17 Steam 更新后的契约重验脚本（主设计 §10 的 5 条断言）

目标：把「游戏一更新就必须重验」的结论写成**按符号名定位、不硬编码地址**的可重跑脚本。

产物：`simplexc/tools/verify_game_contract.py`（新建，只读游戏目录）。

---

## 1. 判据命令与本次结果

```
python simplexc/tools/verify_game_contract.py            # 退出码 0（无 FAIL）/ 1（有 FAIL）/ 2（跑不起来）
python simplexc/tools/verify_game_contract.py --json     # 末尾多打一份 JSON
python simplexc/tools/verify_game_contract.py --only A,C # 只跑某几条
```

本次（buildid `25294874`，exe mtime 2026-09-14，`Turing Complete.exe` 未更新）：

| 断言 | 内容 | 状态 | 关键证据 |
|---|---|---|---|
| A | `compile` 导出 + exe 内唯一调用点 + `flags`=0 | PASS | `compile_real.dll` 导出 3 个；`nimGetProcAddr("compile")` @`0x14021D426` → 指针槽 `0x146249EC0`；经该槽的间接调用全 exe **1** 处（`0x14021D004`）；调用点前最后一次写 r8d 是 `xor %r8d,%r8d`（`0x14021CFF6`），中间无分支 |
| B | `jit` 内 `memcpy(dest, ptr+8, len)` + 尾跳 | PASS | `memcpy` @`0x14048C4B0` 调用点 `0x14021CCE9`，其前 3 条是 `add $0x8,%rdx`；其后 `call c_jit` → `lea (%rsi,%rdi,1),%rax` → `jmp *%rax` |
| C | `handle_request_compile_and_run` 内 out[3] 的 test/jcc 失败分支 | PASS | `test %rbx,%rbx` @`0x14021D083` + `jne 0x14021D198`；目标跳过 `send_command`(`0x14021D091`) 与 `createThread`(`0x14021D0F5`)；槽级污点回溯：被 test 的栈槽 ← … ← Result 基址 `0x20(%rsp)`+`0x18` = out[3] |
| D | `eqdestroy` 内 `testb $0x40,0x7(p)` + `rawDealloc` | PASS | 命中 `eqdestroy___pureZtimes_u2668`（另一个候选用 `u4341` 没有 rawDealloc）；`je` 落空路径有 `ret`、目标到 rawDealloc 之间无 `ret` → 门控方向与「bit62 清位才释放」一致 |
| E | 产物头 8 字节的值仍不被读（除 bit62） | **UNKNOWN** | 见 §4 |

摘要行 `PASS=4 FAIL=0 UNKNOWN=1`。**UNKNOWN 不算通过**。

「定位方式」每条都随输出打印（测试改动前先看那行）。

---

## 2. 五条断言各自的定位方式（设计 §10 只说「按符号名」，这里写实际招数）

| 断言 | 怎么定位 |
|---|---|
| A | `compile` 是**运行时**从 `compile.dll` 取的（`nimLoadLibrary`+`nimGetProcAddr`），exe 符号表里没有它 → 只能「符号 `nimGetProcAddr` + 第二实参字符串 == `"compile"`」找出返回值的落点（`.bss` 指针槽），再全 `.text` 扫「经该槽的间接调用」（`FF 15` 与 `mov slot(%rip),%rXX`+`call *%rXX` 两种形态） |
| B/C/D | COFF 符号名正则（`jit__modelZsimulationZjitZjit`、`handle_request_compile_and_run__…`、`eqdestroy___pureZtimes`、`rawDealloc__system`、`memcpy`）→ 函数边界取 `.pdata` 的 RUNTIME_FUNCTION → 只反汇编那一段 → 指令模式匹配 |
| E | 全 `.text` 字节模式扫描（不做反汇编） |

三条实现约定（写进脚本注释，别再踩）：

1. `_uNNNN` 是 Nim 的声明序号，更新后会变 → 一律可选后缀；`eqdestroy___pureZtimes` 有 2 个符号，所以按「候选 → 逐个验」而不是「找一个」。
2. 符号表 / 节表 / 导出表 / `.pdata` 全部本脚本自己解析 PE。**不复用 `work/symbols/*` 与 `work/function_bounds.tsv`** —— 那是上一次会话的快照，Steam 更新后必然过时，用它等于把旧地址硬编码回来。
3. 只有「需扫全 `.text`」的判据走原始字节（间接调用点计数、memcpy 调用点、`testb $imm,0x7`），其余靠 `objdump --start-address/--stop-address` 只反汇编函数体；不要整段反汇编 exe。

---

## 3. 与设计/事实描述不一致的地方

1. **`flags` 判据不能写成「紧邻 `xor %r8d,%r8d`」**：实际是 `xor %r8d,%r8d`(0x14021CFF6) → 3 条其它指令 → `call`(0x14021D004)。
   脚本写成「调用点前最后一次写 r8d 是置零 **且** 置零与调用点之间无分支（支配）」。
2. **`eqdestroy` 不是唯一符号**：`eqdestroy___pureZtimes` 下有 `_u2668`（有门控，且在 `handle_request_compile_and_run` 里被调用）与 `_u4341`（无 rawDealloc）两个。
3. **`testb $0x40,0x7(p)` 在 exe 里到处都是**（全 `.text` 近万处，Nim 的字符串字面量标志判定被内联进每个 eqdestroy）→ 「扫这个形状」不能当判据，必须按符号定位到具体函数。
4. `jit` 里 `memcpy` 的**越界 8 字节**（读 `out_ptr+8+len`）不在断言清单里，本脚本也没验（要数据流），仍只有事实条目里的反汇编依据。
5. 函数边界用 `.pdata` 而不是「下一个符号」：`jit` 后面的「下一个符号」在符号表里是个节符号，按「下一个符号」会取错（本次实测拿到 0x14021CD40 恰好是 jit_function，但那是巧合）。

---

## 4. 断言 E 为什么只能是 UNKNOWN

「全程序没有别处读头 word」是否定性命题，静态证明要全程序数据流（哪个指针指向产物是运行期性质）。
脚本给的近似判据里：

- 全 `.text` 有 **372** 处「源指针 +8 的 memcpy」调用点，与 jit 那处**指令形状完全相同**；静态分不出哪个指针指向产物 → 这条**不能作为证伪项**（只会永远报警）。
- 唯一的证伪项：全 exe「读头第 7 字节」的 `testb` 立即数是否都是 `0x40`（本次近万处**全是** `0x40`，即没有别的位被 test）→ 没触发。
- 它挡不住「用 `mov`/`movzx` 把整字读走」那类读者，所以**不能据此报 PASS**。

要真判定只有动态：放一份**头字垃圾但置了 bit62** 的产物进游戏跑一轮，看是否崩 / 走错分支；那要部署自研实现，不在本脚本范围。

---

## 5. 判据自检（证明 PASS 不是空过的）

造 6 个字节级变异副本（不改游戏目录，副本落在 `work/tmp/negtest/<名字>/`）：

```
python work/tmp/negtest/make_mutants.py
python simplexc/tools/verify_game_contract.py --game-dir work/tmp/negtest/n1_flags --only A
python simplexc/tools/verify_game_contract.py --game-dir work/tmp/negtest/n2_bit62 --only D
python simplexc/tools/verify_game_contract.py --game-dir work/tmp/negtest/n3_nosym
python simplexc/tools/verify_game_contract.py --game-dir work/tmp/negtest/n4_noexport --only A
python simplexc/tools/verify_game_contract.py --game-dir work/tmp/negtest/n5_gateinv --only D
python simplexc/tools/verify_game_contract.py --game-dir work/tmp/negtest/n6_no_plus8 --only B
```

| 变异 | 期望 | 实测 |
|---|---|---|
| n1 `xor %r8d,%r8d`→`xor %r9d,%r9d` | A FAIL | A FAIL「flags 实参没有置零的写」，退出码 1 |
| n2 `testb $0x40`→`$0x20` | D FAIL | D FAIL，退出码 1 |
| n3 符号表指针清零 | 退出码 2 | 「没有 COFF 符号表」，退出码 2 |
| n4 导出名 `compile`→`compilx` | A FAIL | A FAIL「compile 不在导出表里」，退出码 1 |
| n5 `je 释放路径`→跳回 ret | D FAIL | D FAIL「落空路径里没有 ret」，退出码 1 |
| n6 `add $0x8,%rdx`→`add $0x0,%rdx` | B FAIL | B FAIL「+8 调整没找到」，退出码 1 |

⚠ 这一轮自检改掉了一个**假 PASS**：n6（第一版 B 的兜底形态太松，把 `mov 0x8(%rbx),%rdx`（从 Result 取 out_ptr）当成了 +8 跳过）。教训是「兜底形态」必须能区分「加法/取址 +8」与「从 +8 处取字段」，否则判据退化到只认「有个 memcpy」。
变异副本用完已删（不留别人的二进制副本），`make_mutants.py` 可随时重现。日志在 `work/tmp/negtest/negtest.log`。

---

## 6. 未解决 / 存疑（主程折叠时请一并看）

1. **E 判不了**（§4），需要部署级动态实验才能真判。
2. **C 的 out[3] 认定含一层结构假设**：Result 40 字节落在 `rsp+0x20`，`out[3]` = 基址 + 0x18。脚本靠「`lea d(%rsp),%rX` → `mov %rX,%rcx`（compile 第一实参）」推基址，靠槽级污点回溯认字段。若 Result 布局变，脚本会报 UNKNOWN 而不是 FAIL（不会假 PASS），但需要人工核对。
3. 脚本依赖 PATH 里的 `objdump`（w64devkit binutils）；符号被 strip 时至多只能做到「模式定位」，现在直接退出码 2 并说明原因。
4. **本脚本不覆盖的**重验项（仍是人工/别的判据）：产物逐 tick 状态等价、bit62 置位后不被 free（运行期事实）、`compile` 失败路径要非 0 `err_len`、stdlib 前缀 27711 B 与内建清单。
5. 事实账本里「`flags` 实测恒为 0」现在有第三条来源：本脚本每一次重跑都会重新验一遍（exe 侧），不再只靠 manifest 那 9 行。

---

## 7. 建议折叠进账本的条目（主程决定）

- `docs/facts.md`：给「compile() 的调用契约」补一句验证方式 = `python simplexc/tools/verify_game_contract.py`（断言 A）。
- `docs/facts.md`：给「产物头 8 字节：只有 bit62 被读」「游戏如何执行产物」「失败时不能只把 out_len 置 0」各补一句「可重跑判据 = 同一脚本的 E / B / C」，并注明 **E 恒为 UNKNOWN**（近似判据，不是证明）。
- `docs/facts.md`（可选补充）：`testb $0x40,0x7(p)` 是内联形状、全 exe 近万处；`eqdestroy___pureZtimes` 有 2 个符号 —— 这两条是「按形状扫」会误判的原因。
- `docs/pitfalls.md`：别把「有个 memcpy / 有个 test/jcc」当判据（要判「+8 跳过点」「跳转方向」），否则变异样本能骗过判据（n6 实测）。
- `docs/map.md`：`simplexc/tools/verify_game_contract.py`（Steam 更新后先跑它）+ `work/tmp/negtest/make_mutants.py`（判据自检的变异生成器）。

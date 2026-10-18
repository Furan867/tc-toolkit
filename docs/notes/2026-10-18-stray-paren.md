# 窄修 ②：`return <expr>)` 里多余的 `)` 被吃掉

对应诊断 `docs/notes/2026-10-18-m5-gap-diagnosis.md` 的「② 表达式里出现意外的 ')'」；本文件只记实现与验收。

## 结论

改一处：`simplexc/frontend/parser.cc` 的 `parse_return`，解析完 `return` 的表达式后，若下一个 token 是
`op ')'` 就消费掉。语句位置的通用路径（`parse_primary` 兜底）**没动**，所以
`store(...))` / `x = f(...))` 这类形状照旧报「表达式里出现意外的 ')'」——参考在那里的行为是 fatal。

## 判据

### a. 最小复现（`work/tmp/m5gap/stray_bad.si`，第 2 行 = 100.si:164）

```bash
$ build/simplexc_cli.exe --src work/tmp/m5gap/stray_bad.si --out work/tmp/m5gap/stray_bad.bin
# 改前
语法错误：第 2 行 第 125 列：表达式里出现意外的 ')'          rc=1
# 改后
UNSUPPORTED: 编译期类型参数 Size @ 第 1 行                    rc=1
```

改后不再报括号码位；停在泛型单态化那道坎上（与诊断里 `stray_ok.si` 同一去处）。

**多余括号是 no-op 的直接证据**（对照参考：`ret_ctl.si` 与 `ret_plain.si` 的参考产物同为
`f41b2839b25cf6a9…`）。取同一份 `def`，只差 `- a)` / `- a))`，各自加 `run_sim: while true { f(1) }`
让函数可达：

```bash
$ diff work/tmp/m5gap/chk_ret_nostray.si work/tmp/m5gap/chk_ret_stray.si   # 只差那一个 ')'
2c2
< def f(a: Int) U64 { if a == 1 { return load(<U64>, Ptr 8192 + (128 - ((a + 7) / 8)) - a) } return 0 }
---
> def f(a: Int) U64 { if a == 1 { return load(<U64>, Ptr 8192 + (128 - ((a + 7) / 8)) - a)) } return 0 }
$ sha256sum work/tmp/m5gap/chk_ret_nostray.bin work/tmp/m5gap/chk_ret_stray.bin
a5af7d318200d4a042e8ff2c5daa7cacd47f8f7765d2f6356065b879387aa378
a5af7d318200d4a042e8ff2c5daa7cacd47f8f7765d2f6356065b879387aa378
```

### b. 17 份语料的状态分类（前后对比）

前后各跑一次子集（`--report` / `--tsv` / `--summary-json` 全部指到 `work/tmp/m5/`，不碰 98 份全量报告）：

| | 分类 | 数量 |
| --- | --- | --- |
| 改前 | `cli_fail` · `ParseError: 第 # 行 第 # 列：表达式里出现意外的 ')'` | 17 |
| 改后 | `cli_fail` · `ParseError: 第 # 行 第 # 列：模板字面量的 `{…}` 插值语义未定（U-L3 未决）→ 未支持` | 17 |

改后两次都 `可装配 0 · 逐 tick 全等 0 · cli_fail 17 · 装配回退率 100.0%`；`ParseError` 那一类换了文本，
数量没多也没少 —— 这 17 份的括号行都在插值行之前，所以卡点前移到 ① 插值（下一个任务 #9），不是本次回归。
例：100 改后报 `第 1818 行 第 23 列`（改前是 164 行），与诊断预言一致。

### c. `python simplexc/tools/check.py`

```text
corpus         PASS  0.75s  98/98 rebased_identical · unclassified=0
host_selftest  PASS  0.24s  ticks=3/3 · tick_exact=True
lower_equiv    PASS  0.23s  ticks=10/10 · diff_count=0 · verdict=逐 tick 全等
hook_offline   PASS 19.71s  cases=8/8
probe_006      PASS  0.51s  ticks=3 · identical=True
总计 5 步：5 PASS · 0 FAIL · 0 SKIP
```

## 写死的口径（诊断里没测、语料里也不存在）

- 只吃**紧随表达式的那一个** `)`；再往后还有 token 时，剩下的 token 按下一条语句解析（多半报错）。
- `return <expr>))`（多两个）**仍然报语法错误**：`work/tmp/m5gap/ret_stray_two.si` →
  `语法错误：第 1 行 第 90 列：表达式里出现意外的 ')'`。参考对多两个也是 fatal，方向一致。
- 不吃跨行的 `)`？本实现**不**看行号 —— 条件就是「下一个 token 是 `)`」。语料 17 份的括号都在
  `return` 表达式同一行，这个区别在语料里观察不到；如果将来要收紧，加 `nxt.line == t.line` 即可。

## 遗留风险

- 前一行 `return` 的表达式合法结束、下一行行首是一个游离 `)` 的文件会被静默接受（参考未测；语料无此形状）。
- 括号只是这 17 份的第一道坎，装出来仍需 ① 插值（#9）。
- 本次子集跑第一次没带 `--report`，覆盖了 98 份全量报告，已 `git checkout --` 还原（pitfall 见
  `docs/pitfalls.md`）。
- 与并发写者：`build/simplexc_cli.exe` 由本人重建过一次，无「exe 被截断 / 与语法无关的错」，无需重跑。

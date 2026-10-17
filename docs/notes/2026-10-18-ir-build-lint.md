# 清 pi-lens 债：`simplexc/lower/ir_build.py` 17 条 → 0（产物逐字节不变）

**一句话**：17 条里 11 条是**真的会抛**的调用 → 加 `try/except` 并给「路径 + 原因」的失败解释；
4 条是**规则不适配**（嵌套函数、三态 `is True/is False`、`\d+` 保证的 `int()`）→ 改成显式写法、行为等价；
006 与 rv32im 的产物 md5 改前改后逐字节相同，`check.py` 4 步 PASS，rv32im 强判据差异 0 条。
过程现场全在 `work/tmp/lint0/`；worker 报告在 `work/tmp/agent-reports/ir-build-lint.md`。

## 判据入口：pi-lens 本体的等价替代

本会话里 `pi-lens-analyze` 起不来（`Cannot find package '@earendil-works/pi-tui'`），但 pi-lens 的
**规则文件就在磁盘上**，用 `ast-grep` 跑同一条规则集即可（不装、不猜规则内容）：

```bash
SG=/c/Users/Furan/.pi/agent/npm/node_modules/@ast-grep/cli/ast-grep.exe
# 47 份 `language: Python` 的规则从 pi-lens/rules/ast-grep-rules/rules/ 复制到
# work/tmp/lint0/pyrules/，sgconfig.yml 里 `ruleDirs: [pyrules]`
"$SG" scan --config work/tmp/lint0/sgconfig.yml --json=stream simplexc/lower/ir_build.py
```

**改前跑出来逐条命中主程给的那 17 条**（规则名、行号、数量全一致，`base.jsonl`）→ 这个入口是忠实的。
改后：47 条 Python 规则 = **0 条**，换成 pi-lens 全部 **298 条**规则也 = **0 条**（`after.jsonl` / `after_all.jsonl`）。

## 17 条的分类与修法

| 规则 | 条数 | 真假 | 修法 |
| --- | --- | --- | --- |
| `unchecked-throwing-call-python` | 11 | **真** | 每处加 `try/except`，失败时打印「路径 + 原因」并给非零退出码（见下节） |
| `no-init-return` | 3 | **假**（嵌套函数的 `return`） | `LLVM.__init__` 里的嵌套 `f()`/`call()` 提到模块级 `_bind(lib, name, res, *args)`；`__init__` 里只剩一行 `f = partial(_bind, lib)`（一个 `return` 也不留，`f(...)` 的 70 多个调用点一个字没动） |
| `no-identity-operator-on-literals` | 2 | **假**（对 bool 单例的 `is` 是稳的） | `if decided is True: … / if decided is False: …` → `if decided is not None: if decided: … else: continue`。`in_scope_const()` 只返回 `True`/`False`/`None`（`return arg.text in self.defs`），两版等价 |
| `unchecked-numeric-parse-python` | 1 | **假**（`SIZED_RE = ([US])(\d+)` 保证是数字） | 仍按批准的方向加保护：`int(digits)` + `except ValueError → Unsupported(f"位宽 {name}（{digits!r} 不是十进制数）")`。这条 `except` 目前**不可达**，留着只为「形状变了别抛裸 ValueError」 |

## CLI 错误信息（11 处，判据 5 的那种）

规则只认 `try_statement`，所以 `contextlib.suppress(...)` 那两处也改成了 `try/except`（省略号语义不变）。
失败一律「路径 + 原因」+ 退出码：`2` = 环境/参数（建目录、写 IR/COFF），`1` = 编译/装配/比对失败。

```text
$ python simplexc/lower/ir_build.py --src no/such/file.si --out work/tmp/lint0/x.bin
读不了 no/such/file.si
rc=2

$ … --workdir work/tmp/lint0/afile            # afile 是个普通文件
建不了中间目录 work/tmp/lint0/afile：[WinError 183] 当文件已存在时，无法创建该文件。: 'work/tmp/lint0/afile'
rc=2

$ … --out work/tmp/lint0/afile/x.bin          # obj 写成功之后才碰产物目录
建不了产物目录 D:\…\work\tmp\lint0\afile：[WinError 183] 当文件已存在时，无法创建该文件。: '…\afile'
rc=2

$ … --out work/tmp/lint0/f/006.bin            # f/006.bin.entry 是个目录
装配失败：读不了 D:\…\f\006.bin.entry（装配器写出的 entry 文件）：[Errno 13] Permission denied: '…\006.bin.entry'
rc=1

$ … --check --ref no/such.bin
找不到 006.si 的参考产物（corpus/ref + manifest.tsv）
rc=2
```

**不吞错**：`host_run` 读不了 host 输出时只打印警告、返回空快照 —— 上层「tick 数不够」判据立刻把它变成
`rc=1`；`resolve_ref` 读不了 manifest 时返回 `(None, None)`，主程那段既有的「找不到参考产物」接着报 `rc=2`。
半截行（host 被杀）仍按老口径跳过：假 host 写「2 条好行 + 1 条半截行」→ 取到 2 条、不崩
（`work/tmp/lint0/test_error_paths.py`）。

## 产物对照

| 判据 | 改前 | 改后 | 期望 |
| --- | --- | --- | --- |
| 006 blob md5 | `9d888a415cc7881de2110311485fbc1f` | `9d888a415cc7881de2110311485fbc1f` | 同左 |
| rv32im blob md5 | `c043fd1b48edab296ac3cc9a2044beb7` | `c043fd1b48edab296ac3cc9a2044beb7` | 同左 |
| 006 blob 字节 / entry | 3508 B / `0xa90` | 3508 B / `0xa90` | 不变 |
| rv32im blob 字节 / entry | 58612 B / `0xe1f0` | 58612 B / `0xe1f0` | 不变 |

改后逐条判据（现场：`work/tmp/lint0/verify_*.log`）：

```text
1) ast-grep（pi-lens 规则）：47 条 Python 规则 0 条 · 298 条全部规则 0 条
2) md5：9d888a415cc7881de2110311485fbc1f / c043fd1b48edab296ac3cc9a2044beb7
3) python simplexc/tools/check.py：4 步 4 PASS · 0 FAIL · 0 SKIP
4) rv32im ours vs 参考产物（两边 --seed-reset-data seq、10 tick、entry 0x215B vs 0xe1f0）：
   差异 0 条 · regions/pages 逐 tick 全等 · DATA 段哈希 6165fb5d…（= facts.md 那条非平凡图案值）
5) --src 不存在：`读不了 no/such/file.si` + rc=2（无裸 traceback、无静默成功）
```

## 还剩什么

1. **pi-lens 的 tree-sitter query 档没动**（`python-path-traversal`，severity=warning/security）：
   本文件里 6 处 `open(动态路径)` / `os.remove` 改前就在。调用点多重集比对证明本次**只变了一处调用**：
   `int(m.group(2))` → `int(digits)`，没有新增文件 API 调用点（只有行号位移，所以逐编辑 delta 会把它
   列成「新增」）。要清零它得改 CLI 的路径构造（`normpath`/allowlist），与本次「只改这 17 处」的范围冲突
   —— 需要主程先定口径。
2. **ruff 档不在本次检查面**：本机没有 ruff 二进制。若主程口径含 ruff，`try/except … pass` 那类写法要复核
   （本次两处空 `except` 体写的是 `continue` + 注释，不是 `pass`）。
3. **`--src` 不存在的消息不含原因**（还是「读不了 `<path>`」）：判据 5 过，但不在这 17 条里，没动。
4. **未测到的分支**：`host_run` 里 `open(out)` 抛 `OSError` 的那条（要一个「`isfile` 为真但读不了」的文件）、
   `resolve_ref` 里 manifest 读不了的那条（不能动语料目录）。两条都只是「打印路径 + 原因」，没别的东西。

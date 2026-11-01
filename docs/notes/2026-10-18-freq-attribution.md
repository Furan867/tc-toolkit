# 2026-10-18 watch/bench 的产物归属编号错位（`impl=ours` 那几趟）

## 结论

`impl=ours` 那几趟编译**不经改写器**（`compile_hook.c`：`if (!ours_ok) optimize_inplace(...)`），
于是既没有 `NNN.opt.bin`、也没有 `cfg`/`opt`/`writeback` 行 —— 而 `tools/freq/products.py`
把「产物」等同于 `work/opt/NNN.opt.bin`。后果三处，全是同一个假设：

| 症状 | 现场 |
|---|---|
| watch 总结把 ours 轮标成上一轮的编号 | `summary_ours_2.txt` 记成 `#008`，实际 seq 009（M4） |
| 运行中出现的 ours 轮编译点被漏掉 | `scan_products` 不报它 → 段落沿用旧编号，15s 后还误报「改写后产物一直没出现」 |
| bench 的 `--variant ours:off` 必然超时 | `work/tmp/m4/ab2.json`：重试 3 次后「等了 60s 没等到本次编译的产物」 |

产物实际上就在 `work/hook/hook_out_NNN.bin`（转发层捕获，`manifest.tsv` 的 `out` 列同指），
`hook.log` 的 `impl` 行（每次编译必写，含 seq）已经足够判定它是哪一趟、走的是哪个实现。

## 修复点

`tools/freq/products.py`

- 新增 `done_rounds()`：编号 → **终态行**（`impl` 且 `result=ok`，或 `writeback`/`opt`）。
  「编译写完了」的权威信号是「终态行落了」，不是某一条特定的行。
- 新增 `compile_points()`：编译点 = 产物已写回，两条腿 —— `NNN.opt.bin`，或
  **终态行是 `impl`** 的那趟的 `hook_out_NNN.bin`。编号按整数序，带回 mtime。
- `latest_product()` / `scan_products()` 改为建立在 `compile_points()` 上；
  `product_path()` / `product_fp()` 改为「写回游戏的那份」：`opt.bin` 优先，否则 `hook_out_NNN.bin`。
- `wait_new_product()` 的进度行不再说「等改写子进程」（ours 那趟没有这东西可等），
  改成按 `done_rounds()` 说事实：「#NNN 的终态行已落，等产物文件就位」/「编译已触发，等这一趟写完」。

`tools/freq/report.py`：区间表的 rewrite 列加了第三档 `—（impl=ours）` ——
`opt` 行 → `cfg` 行 → 都没有时归因给 `impl` 行（不再只印一个 `?` 让人以为是改写器失败）。

## 归属判据（怎么保证没绕过 facts 的纪律）

- **编号与实现**只从 `hook.log` 读：`impl` 行的第 2 列（seq）+ `impl=`/`result=` 键。
  不看 `opt.ini` 当前值（那正是 `docs/facts.md` §产物 rewrite 模式的权威来源记下的坑），
  也不看 `manifest.tsv`（它是同一件事的另一份抄写，读了等于把判据复制一遍）。
- `ours` 轮没有 `opt` 行也能取到正确编号：`impl` 行**每次 `compile()` 都写**（含 reference，
  回退率要分母），所以它是日志里「编译发生过」的完整清单；`cfg`/`opt` 只是其中走过改写器
  那些趟的补充。终态行是 `impl` 这一事实本身就是「这趟不过改写器」的信号，无需猜测。
- 产物文件只当「这份机器码在哪」，不当归属证据；存在性检查同时兜住「日志有记录但产物没写回」
  （崩在写回之前：`impl=ours` 但没有 `hook_out`）。

## 复现与前后对照

`work/tmp/attr/replay.py`（一次跑完四节判据，任一节不等就 `exit 1`）：
① 起始编译编号、② 运行中出现的编译点、③ 与现场记录的两份 `summary_*.txt` 对照、
④ bench 的等产物判据（用 `work/hook`+`work/opt` 的**现状快照**，真数据 seq 005=reference、
006/007/008=ours）。

```
python work/tmp/attr/replay.py > work/tmp/attr/before.txt   # 修复前：rc=1，4 处错位
python work/tmp/attr/replay.py > work/tmp/attr/after.txt    # 修复后：rc=0
```

修复前（`before.txt`）—— ours 轮标成上一轮，reference 轮正确：

```
00:16:41 起始编译 #006  期望 #007  ✗ 错位      →  1) #006 24.843  41 ... 69a94452  0  off
00:19:37 起始编译 #008  期望 #009  ✗ 错位      →  1) #008 24.818  41 ... 69a94452  0  off
00:18:37 起始编译 #008  期望 #008  OK          →  1) #008  5.204  41 ... 69a94452  0  off
00:20:34 起始编译 #010  期望 #010  OK          →  1) #010  5.169  41 ... 69a94452  0  off
编译 #006 (ours)  捕获=['006'] 编译点=[]  漏掉 → 段落沿用上一轮编号，且 15s 后会误报「改写后产物一直没出现」
```

修复后（`after.txt`）：

```
00:16:41 起始编译 #007  期望 #007  OK   →  1) #007 24.843  41 ... aebf07e4  -  —（impl=ours）
00:19:37 起始编译 #009  期望 #009  OK   →  1) #009 24.818  41 ... aebf07e4  -  —（impl=ours）
00:18:37 起始编译 #008  期望 #008  OK   →  1) #008  5.204  41 ... 69a94452  0  off
00:20:34 起始编译 #010  期望 #010  OK   →  1) #010  5.169  41 ... 69a94452  0  off
编译 #007 (ours)  捕获=['007'] 编译点=['007']  报出
```

指纹也跟着归位：修复前两行都是 `1099160c`（上一轮 reference 产物的 sha），修复后 ours 轮
是 `aebf07e4` = 当时那份 ours 产物（58612 B，md5 `513a50bb…`）的 sha256 前 8 位。

第四节（bench 判据，真数据快照）：

```
轮 #005 impl=reference  现判据 → 005  OK    旧判据（只扫 opt.bin）→ 005  OK
轮 #006 impl=ours       现判据 → 006  OK    旧判据（只扫 opt.bin）→ 005  ✗ 不认这轮 → 会等满超时
轮 #007 impl=ours       现判据 → 007  OK    旧判据（只扫 opt.bin）→ 005  ✗ 不认这轮 → 会等满超时
轮 #008 impl=ours       现判据 → 008  OK    旧判据（只扫 opt.bin）→ 005  ✗ 不认这轮 → 会等满超时
```

「旧判据」列是把修复前 `latest_product()` 的函数体照抄进 harness 得到的对照
（`git` 里没有这段历史：`tc-toolkit/**` 被 `.gitignore` 挡在仓外；真机现场见 `ab2.json`
与当时的终端输出）。①②③ 的「修复前」是**同一份 harness 在改代码之前**跑出来的真实输出。

真数据直查（只读，游戏在跑）：

```
$ python -c "from tools.freq import products; p=products.latest_product(); print(p.num, p.path.name, products.product_fp(p.num))"
008 hook_out_008.bin 5362038c        # = sha256(work/hook/hook_out_008.bin)[:8]
$ ... products.compile_points()  →  001..005 = *.opt.bin，006/007/008 = hook_out_*.bin
$ ... products.done_rounds()     →  001..005 = writeback，006/007/008 = impl
```

## 需要壳配合吗

**不需要**。`impl` 行本来就带 seq（第 2 列）与 `result=ok`，`hook_out_NNN.bin` 也照旧落盘，
判据所需的三样（编号、实现、写回产物）全在。唯一要守住的是：ours 分支继续 `dump("out")`
（`compile_hook.c` 里 `o_wrote` 那段），以及 `impl` 行继续每次调用都写 —— 这两条一破，
归属就无从谈起（`impl` 行缺失时本层的退化行为是「不知道」，不会瞎猜）。

## 判据命令

```
python tools/freq/test_freq.py        # 55 tests OK（新增 CompilePointTest / DoneRoundTest / WaitNewProductTest）
python tools/watch_freq.py --help     # 正常
python tools/bench_rewrite.py --help  # 正常
python work/tmp/attr/replay.py        # rc=0（修复前 rc=1）
```

真机上 `watch_freq` 对 ours 轮的期望输出（`#009` + 指纹 + `—（impl=ours）`）由 harness 覆到了，
本次没有在真游戏上重跑采样（不介入主程的游戏会话）。

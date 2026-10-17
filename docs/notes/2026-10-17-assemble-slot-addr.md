# 装配器桩/槽定址：`__imp_` 槽地址被后追加的桩块挤歪（2026-10-17）

结论：`assemble()` 里「收集要造哪些桩/槽」与「造并定址」拆成两趟，地址一次定死；返回 Ok 前
再逐条自校验 `<blob>.stubs` 的内容（`res.stubs`）与产物字节一致，不一致就 `BadObject` 失败。

## 症状（另一个 agent 实测，本次只复核了机制）

`work/hook/hook_src_040.si` 的第一版（`dllimport` 写法）装配出的 `<blob>.stubs` 说槽在
`ae0/ae8/af0`，真身在 `afc/b04/b0c`；宿主按 sidecar 去 patch → 崩在 `entry+0x14E`。

机制（旧代码 `place_for`）：某个符号第一次被引用时**当场**按
`body_end + stubs.size() + slots.size()` 定它的落点。槽排在全部代码桩之后，而「桩总共多长」
要到扫完全部重定位才知道 —— 之后扫到的裸名 extern 再往桩块追加字节，`stubs.size()` 变大，
已记下的槽地址（以及引用它的 REL32）整体前移。

## 最小复现（`work/tmp/slotfix/`，不用游戏）

`trigger.c`：先 `__declspec(dllimport) Sleep`（→ `__imp_Sleep` 数据槽），再裸名
`GetSystemTimePreciseAsFileTime`（→ 代码桩），且前者在 `.text` 里偏移更小。

```
build/llvm/bin/clang.exe --target=x86_64-pc-windows-gnu -O2 -ffreestanding -fno-stack-protector \
    -c -o work/tmp/slotfix/trigger.obj work/tmp/slotfix/trigger.c
work/tmp/slotfix/tcblob_old.exe --obj work/tmp/slotfix/trigger.obj --entry tc_main \
    --out work/tmp/slotfix/old/trigger.bin          # 修复前的 build/tcblob.exe 副本
build/tcblob.exe --obj work/tmp/slotfix/trigger.obj --entry tc_main \
    --out work/tmp/slotfix/new/trigger.bin
python work/tmp/slotfix/check_sidecar.py --blob <blob>   # 判据脚本
```

| | 旧装配器 | 新装配器 |
|---|---|---|
| sidecar | `48 slot __imp_Sleep` / `48 stub GetSystemTimePreciseAsFileTime`（**两条同偏移**） | `48 stub …` / `54 slot __imp_Sleep` |
| 校验脚本 | 对不上 1 条（槽那条读到的是桩的字头，值 `0x…b848`） | 全部对上，rc=0 |
| `--run` | 段错误 rc=139 | `RUN ok`，线程退出码 0 |
| 与旧 blob 的差异 | — | 只差 1 字节：`0x3D` 的 imm32 `0x0F` → `0x1B`（`jmp *[rip+imm]` 的目标） |

`check_sidecar.py` 核两件事：形态（stub = `48 B8 <imm64> FF E0`；slot = 8 字节）与数值
（该符号在 kernel32 的导出地址）。数值可比的前提是 **kernel32 基址每次开机随机一次、同一次开机内
跨进程相同**（实测两个 python 进程一致）；脚本对此有兜底：所有条目差同一个非零值就只报形态结论。

## 防复发：返回前的自校验

`assemble.cc` 第 5 段用 `res.stubs`（就是 sidecar 会写出去的那份数据）逐条比对 payload：
偏移越界、形态不符、imm64/槽内容 ≠ resolver 给的地址 → `AsmStatus::BadObject` + detail 写明
「哪个符号、哪个偏移、产物该处是什么、应为是什么」。

变异测试证明它真的会拦（`work/tmp/slotfix/mut/assemble_mut.cc` = 把旧那一行
`p.addr = body_end + slots.size();` 塞回去再编一份 tcblob）：

```
装配失败（status=1）：自校验失败：__imp_Sleep 的槽在 0x48，产物该处是 48 B8 …（应为 40 FE …）—— sidecar 与产物不是一回事
```

即：产物与 sidecar 一旦不一致就装配失败，不再发出一份对不上的产物。

## 契约（保持不变的）

- `AsmResult::stubs` 的顺序仍是**首次引用顺序**（sidecar 写出去的顺序与旧实现逐字节相同）。
- 物理布局仍是「全部代码桩（16 对齐）→ 全部 `__imp_` 槽（8 对齐）」。
- 未解析符号仍是 `UndefinedSymbol` + **第一个**未解析的名字。

## 判据（本次实跑）

- `python simplexc/tools/check.py`：4 步全 PASS。
- `python simplexc/tools/probe_006.py --ticks 10 --no-throughput --quiet --outdir work/tmp/slotfix/p
  --results work/tmp/slotfix/pr`：10/10 tick、`identical=true`、`semantic_diffs=[]`。
- spike：**没跑 `build.py spike`**（会覆盖主程正在游戏内使用的 `build/blob_empty.bin`）。
  改用同一条 tcblob 命令把空程序产物写到 `work/tmp/slotfix/spike/blob_empty.bin`：
  sha256 `3b378baa…` == `build/blob_empty.m1.bin`（逐字节相同）== 判据给的期望值；
  `build/blob_empty.bin` 全程 sha 未变（`3e776d1485…`）。
- `equiv_build.py --src work/hook/hook_src_040.si --bin work/hook/hook_out_040.bin --entry 0x212F
  --workdir work/tmp/slotfix/equiv --mark`：10/10 tick 全等、0 差异；
  `ours.bin` sha `b61342ff…`、`ours_marked.bin` sha `3e776d14…` —— 与修复前的旧值相同。

为什么这次 sha 不变：那份捕获的**裸名 extern 引用排在 `__imp_*` 之前**（sidecar 顺序是
stub/stub/slot/slot/…），槽定址时桩已经全部在 `stubs` 里，旧的增量公式恰好给出正确值；两趟写法在
这种输入上布局逐字节等价。踩坑的是相反的顺序。

顺带核到：`build/blob_empty.bin`（主程游戏内验证用的那份）与本次新装配器产出的
`work/tmp/slotfix/equiv/ours_marked.bin` **逐字节相同**，`.stubs` 也相同 —— 即主程手上那份产物
就是修复后装配器的输出。

## `selftest.py --entry`

原先 `selftest.py` 不把入口偏移传下去，捕获的产物（不在 `corpus/manifest.tsv` 里）跑不了，
host 报「在 manifest 里找不到」。现在加 `--entry` 透传（`run_host(..., extra)` 那条既有通路），
判据逻辑一行未动。

## 未解 / 存疑

- 触发这份 bug 的那对捕获（`work/hook/hook_src_040.si`）
  在 16:48 被主程的新捕获从 `work/hook/` 轮换走了，磁盘上已无副本（`work/hook/all/` 里那份 040 是
  另一次会话的，入口不同）；它的输出留在 `work/tmp/slotfix/equiv/`。等价复跑那一步现在只能用
  当前捕获代替（`work/hook/hook_src_001.si` + `hook_out_001.bin`，entry 从同目录 manifest 的 `r2` 取）。
- 自校验只覆盖桩/槽这两块；节内容与 REL32 补丁结果没有同类核对（不是这条 bug 的面，没做）。

# 接口对齐（代码侧）：assemble 的 TooLarge + 宿主的 movabs 硬错误（2026-10-17）

## 结论

接口冻结版 1 §4/§6 的两处代码缺口已落地：

1. `assemble()` 的 `out_len > max_blob` 返回 `AsmStatus::TooLarge`（原来错用 `BadObject`）；
2. 宿主对「地址表外 + 本进程已映射」的 movabs 立即数硬错误退出（原来只统计）。

**派活描述与现状不符一处**：主程的任务说明写「枚举定义已在 assemble.hh（…TooLarge）」，
实测 `assemble.hh:48` 当时是 `{ Ok, BadObject, UndefinedSymbol, UnsupportedReloc }` —— **TooLarge 不在头里**。
主程裁决（本轮内）为「只追加 `TooLarge` 到枚举末尾，不动任何现有成员」，本次照此执行；
追加后 `-c assemble.cc` 通过，且与冻结版 §4 的枚举形状逐字一致。

## 改了什么

| 文件 | 改动 |
|---|---|
| `simplexc/assemble/assemble.hh` | `AsmStatus` 末尾追加 `TooLarge`（现有成员名字/顺序/语义不动，计 1 行） |
| `simplexc/assemble/assemble.cc` | 上限分支 `BadObject` → `TooLarge`（计 1 行；其余 `BadObject` 用法未动） |
| `simplexc/tools/host/host.py` | 复用 `classify_abs_addrs.query_mapping`（经 `abs_scan.CAA`）加 `mapped_region()`；`_scan_product` 收集 `(偏移, 值)` 后调 `_reject_mapped_movabs()` |

## 宿主这一处的两个判断点（写下来免得下一轮重查）

- **`query_mapping` 不足以直接当「已映射」**：它对未分配区间返回成功（`State=MEM_FREE`、`AllocationBase=0`，
  已在 `query_mapping` 内挡掉），但 **MEM_RESERVE-only 区间也返回非 None**（实测 `Protect=0`、`Type≠0`）。
  判据取「`Protect ≠ 0`」＝已提交；能解引用的区间必然如此。
- **检查放在 `_map_game_memory()` 之前**（`_scan_product` 内）：否则宿主自己按地址表摆出来的
  5.8 MB 假内存会先被映射，落在游戏地址邻域外的大整数常量就可能假阳性。
- 硬错误路径 `raise SystemExit(2)`，发生在起线程之前；`--quiet` 也照打（判据不能因安静模式消失）。

## 判据（全部实跑，输出见 `work/tmp/iface/evidence.txt`）

1. `D:/bin/w64devkit/bin/g++.exe -std=c++17 -O2 -c simplexc/assemble/assemble.cc` → rc=0；
   `build/tcblob.exe` 用同一套命令重编（`tcblob.o` + `assemble.o` 链接）→ rc=0，
   重编后的 `tcblob` 装配 spike 三件套得 `sha256 = 3b378baa…7472`，与 `build/blob_empty.bin` 及 M1 记录逐字节相同。
2. TooLarge 实测（`work/tmp/iface/too_large_driver.exe`，rt_min.o，out_len=632）：
   `--max-blob 631` → `status=TooLarge`（rc=0）；`--max-blob 632` → `status=Ok`（边界，rc=0）；默认 64MB → `Ok`。
3. 正常路径不回归：005/006/020/120 各 `--ticks 1` → 四个都 `tick 1: ok`，verbose 仍是「未解释 4」。
4. `host/selftest.py --src 006.si --bin ref/006.bin --ticks 3` → rc=0（逐 tick 全等、工作页 1 个）。
5. 硬错误实测：两份 patch 产物（`work/tmp/iface/patch_movabs.py` 按值定位 movabs，只改第一处）：
   - `005_fixed.bin`：`0x2540BE400`（机器码 +0x7E0）→ `0x7FFE0000`（**固定地址、无 ASLR** 的
     KUSER_SHARED_DATA 兼容页）→ host rc=2，报「+0x7E0、值 0x7FFE0000、私有区 … 区间 0x7FFE0000+0x1000」；
   - `005_dlltry.bin`：`0x2540BE400` → 「上一次 host 运行观测到的 DLL 基址 + 0x1000」→ host rc=2，
     报「compile_real.dll alloc_base=0x…」；同一份产物跑 `selftest.py` → rc=1（宿主非 0 → 判据全灭）。
   - ⚠ `compile_real.dll` 基址**每次进程都重掷**（实测连续两次 host 运行就不同），所以带文件名的那个
     分支要「先跑一次读基址、紧接着 patch 再跑」才命中（本轮第 1 次就命中）；写死进产物的那一条会失效。
     稳定判据只能用固定地址那个（`0x7FFE0000`）。

## 未解决 / 存疑

- 只覆盖「已提交但 `PAGE_NOACCESS`」也算硬错误（判据取 `Protect≠0`）。这类地址不会静默读错（读就 AV、
  由 VEH 抓），但当前一律当硬错误 —— 与「宁可吵」一致，先这样。
- `0x7FFE0000` 是 32 位 KUSER_SHARED_DATA 页，在本机 64 位进程里也映射（实测 `protect=2`）；
  换 Windows 版本可能变 —— 它只用于判据，不进产品代码。
- 宿主进程自己映射的地址是否可能与被判为「表外」的常量撞车（假阳性）没做统计；真撞了表现为宿主拒绝运行，
  不会静默错。

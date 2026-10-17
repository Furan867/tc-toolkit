# 装配器把无原始数据的节（.bss）当文件数据读进了 blob（2026-10-17）

## 结论（先看这个）

1. **根因与修复**：`simplexc/assemble/assemble.cc` 的 `parse_coff` 里，读节内容的判据只有
   `if (raw_size)`，没有看 `PointerToRawData`。`.bss` 是 `SizeOfRawData=8, PointerToRawData=0`
   （文件里没有这个节的数据），于是「从偏移 0 读 8 字节」读到了 **COFF 头**
   `64 86 05 00 <TimeDateStamp>`（Machine + NumberOfSections + TimeDateStamp）。
   改成 `if (raw_size && raw_ptr)`：无原始数据的节 `data` 留空，节在产物里占的字节数记在
   新的 `Section::size`（= `SizeOfRawData`，对象文件里 `VirtualSize` 恒为 0），
   payload 本来零初始化，所以那块就是 `00*8`。
2. **两个后果都消掉了**：产物不再带编译时间戳（两次编译的 `.obj` 只差 4 字节时间戳 → blob 逐字节相同）；
   `.bss` 初值是真 0 而不是垃圾。
3. **布局一个字节都没动**：修复前后同一份 `.obj` 的 blob 长度都是 2164，只有文件偏移
   2088..2095 那 8 字节从 `64 86 05 00 ca 23 d3 6a` 变成 `00*8`（实测 diff 位置 = {2088,2089,2090,2092,2093,2094,2095}，
   2091 本来就两边都是 0x00）。symbol 地址、桩地址、`entry` 全部不变。
4. `M1` 的 728 字节空程序 blob **没被污染**（它的三个 `.obj` 里 `.bss` 的 `rawsize==0`），
   `build.py spike` 重跑后 sha256 仍是 `3b378baa…7472` —— 与 M1 记录相同。

## 与任务书的偏差（一处，是事实更正，不是新决定）

任务书写的是「布局与大小照旧用 `vsize`」。**实测 `vsize`（VirtualSize）在对象文件里是 0**：
clang 与 gcc 编出来的 `.o`，每个节的 `VirtualSize` 字段都是 0，大小在 `SizeOfRawData`。

    $ python work/tmp/fixbss/coff_sections.py work/tmp/fixbss/post/a.o
    [ 3] .bss       vsize=0        rawsize=8        rawptr=0
    $ D:/bin/w64devkit/bin/objdump.exe -h work/tmp/fixbss/post/a.o
      2 .bss          00000008  0000000000000000  0000000000000000  00000000  2**3
                      ALLOC

所以按字面「用 vsize」会得到 `.bss` 大小 = 0。那样做的实际后果**不是**「少 8 字节的垃圾」，而是：
`.bss` 退化成零长度 → `body_end` 前移 8 字节 → `.bss` 里唯一的符号 `last_command_id`
（`sec 3`，`value 0`）落在 `body_end` 上，而桩区正好从 `body_end` 开始 ——
写一次 `last_command_id` 就把第一个桩（`Sleep`，code 偏移 0x830）的 `movabs rax, imm64` 覆盖掉。
修复前装配器**实际上**一直用 `data.size()`（= `raw_size` = 8）当 `.bss` 大小，所以
「用 `SizeOfRawData`」= 保持原布局，只有那 8 字节的内容变干净。判据 d 里
`spike` 的 sha 不变、判据 b 里 `out_len` 不变（2164）都印证了这一点。

## 判据（全部当场跑过）

判据脚本在 `work/tmp/fixbss/`（临时，不进 git）：
`coff_sections.py`（列节的 vsize/rawsize/rawptr）、`coff_blob_check.py`（blob 里搜 COFF 头/时间戳）。

编译（照抄 `docs/notes/2026-10-17-m1.5-probe.md` 的命令）：

    build/llvm/bin/clang.exe --target=x86_64-pc-windows-gnu -O2 -ffreestanding \
        -fno-stack-protector -c -o <a.o|b.o> simplexc/probe/006_equiv.c     # 两次之间 sleep 3
    build/tcblob.exe --obj <a.o|b.o> --obj build/objs/entry_shim.o --obj build/objs/rt_min.o \
        --entry __tc_entry --out <a.bin|b.bin>

| 判据 | 结果 |
| --- | --- |
| a. 同一 `.obj` 装配两次 | `d217850a0a31e967d967497b2d017dd74c818390e499fde96918dbf0ce5f8701`（3 份相同，`cmp` rc=0） |
| b. 修复前（对照）：两个 `.obj` 只差 offset 4 | blob sha 不同：`53c96bdb…b9642` vs `3605d40a…47be0`（`cmp -l` 全文件只差 offset 2092） |
| b. 修复后：同一对 sha 组合 | 两个 blob 都是 `d217850a…5f8701`，`cmp` rc=0；`.obj` 仍只差 offset 4（`cmp -l a.o b.o` → `5 74 77`） |
| c. blob 卫生 | 修复前：`TimeDateStamp` 命中 1 处（`0x82c`）、COFF 头命中 1 处（`0x828`）；修复后：**0 命中**，`blob[2088:2096] = 00*8` |
| d. 不回归 | `python simplexc/tools/check.py` → 4 PASS / 0 FAIL / 0 SKIP；`python simplexc/tools/build.py spike` → `3b378baa…7472`（= M1 记录） |
| e. M1.5 探针重跑（默认 results 目录） | 两变体都 10/10 tick 全等、差异 0、白名单空；blob sha：fold `d217850a…`、hot `3565c3383f56a1c0b80051a4599c09a7813b39c299ad0b7a48b7baca674c0ad5` |

判据 e 的吞吐（paired 口径，1e9 cycle 同驱动）：

| 变体 | 新 | 旧（同一探针、修复前） |
| --- | --- | --- |
| fold 每 cycle | 1.7062 ns（= 参考的 124.5%） | 1.7384 ns（127.9%） |
| hot 每 cycle | 0.46722 ns（快 2.954×） | 0.47499 ns（2.892×） |

差异 ~2%，落在探针记录的噪声范围内（同一 N 配对差、`a·k` 项已抵消），
**结论没变**：直译版仍慢于参考，`hot`（热计数器进寄存器）才是收益来源。

一处容易误读的数字：静态指令数从 fold 517 → 508、hot 551 → 543。
产物机器码字节**逐字节相同**（除那 8 字节），变的是 `objdump -D -b binary` 的**线性反汇编**结果 ——
它以前把 `.bss` 那 8 个垃圾字节当事令解，解码位置被带偏，后面的指令条数因此不同。

## 同类形态的其它节（任务第 3 项）

- `build/objs/{rt_min,entry_shim,prog_empty}.o`：**没有** `rawptr==0 && rawsize>0` 的节，
  也没有 `rawsize==0 && vsize>0`。三者的 `.bss` 都是 `rawsize=0, rawptr=0`（gcc 不占）→ 空程序 blob 不受这个 bug 影响。
- clang 编的 `006_equiv.o` 只有 `.bss` 是这种形态（`.text/.data/.debug$S/.llvm_addrsig` 都有 `rawptr!=0`）。
- 装配器里读节数据的地方**只有** `parse_coff` 这一处（已改）。
- `simplexc/tools/{abs_scan,verify_game_contract,classify_abs_addrs}.py` 也解析 COFF/PE 节表，
  但它们读的是**游戏镜像**（`.bss` 在镜像里是 `rawsize=0`），且 `va2off` 已有
  `delta < s.rawsize` 的护栏 —— 不是同一形态的坑，未改。

## 未解 / 待主程处理

- 这份判据目前是临时脚本（`work/tmp/`）。要长期防回归，得把它折进 `simplexc/tools/check.py`
  的一个步骤（例如「同一 `.obj` 装两次 byte-identical + blob 里不含输入 `.obj` 的 COFF 头」）——
  按 `AGENTS.md` 的归属，那属于判据入口，不在本次允许改的范围内。
- `simplexc/probe/results/` 已按任务书覆盖（旧 blob sha 本来就因这个 bug 不可复现）。

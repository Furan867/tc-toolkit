# pi-lens `cpp-no-printf` 清零：`lower_selftest.cc` + `tcblob.cc`（2026-10-18）

任务：把两个文件的 `cpp-no-printf` 诊断清零，**不绕规则、不降级行为**。
范围：`simplexc/lower/lower_selftest.cc`（26 处）、`simplexc/cli/tcblob.cc`（5 处）——
`simplexc/cli/simplexc_cli.cc` 的 2 处归另一个 worker，本文件一概未碰。

## 改法（31 处）与分类

| 类别 | 处数 | 位置（改前行号） | 做法 |
|---|---|---|---|
| 纯文本 / 顺序拼接（无宽度、无进制） | 26 | selftest 的 65/68/651/656/659/672/678/679/712/721/739/741/784/788/855/859/864/867/873/877/880/890/892；tcblob 的 122/189/216 | `std::cout << a << b << …`，`%s` 配 `std::string` 直接 `<<` |
| 带宽度对齐 | 1 | selftest 676（`   %-10s raw=%6u  reloc=%3u`，含紧随的 678/679 两个收尾调用） | 局部 `std::ostringstream` + `std::left/std::right/std::setw` |
| 带十六进制 | 3 | selftest 746（`+0x%llX`）、777（`entry=0x%llX`）；tcblob 175（`entry=0x%llX`） | 局部 `std::ostringstream` + `std::hex` + `std::uppercase` |
| `%p` | 1 | tcblob 119 | `std::cout << static_cast<const void*>(dest)` ← **唯一形式差异**，见下 |

选择「局部 `ostringstream`」而不是 `cout << std::hex …` 的原因：`left`/`hex` 是**持久状态**，
一旦落在全局 `cout` 上就会污染后面所有输出；局部流用完即弃，不依赖「记得恢复」。
`setw` 只作用于下一次输出，所以每个数字前都单独再设一次。

## 有意**没有**动的地方

- `tcblob.cc` 写 `<blob>.entry` / `.stubs` 的 `fopen/fprintf/fclose` 整段（判据依赖，且未被诊断报出）；
- 两个文件里 stderr 的 `std::fprintf(...)`（未被诊断报出；`cpp-no-printf` 只匹配 `std::printf`，
  判据列表里 tcblob 全部 17 行 `fprintf` 一条都没报，方向已被主程的 33 条清单确认）；
- `lower_selftest.cc` 773/779 行的 `std::snprintf(buf, …)`：它把消息拼进 `char buf` 再 `bad(buf)`，
  同样未被诊断（26 条清单里没有这两行），改动它属于超范围。它的 `%llX` 实参都带
  `(unsigned long long)` 转换，没有类型不匹配。
- 没有引入 `using std::printf`、宏别名、`NOLINT`、`#pragma`：`grep -nE "using std::printf|#define .*printf|NOLINT|pragma GCC diagnostic|noqa"` → 无匹配。

## 判据

### P1 诊断清零（**替代判据**：本 worker 的工具白名单里没有 `lens_diagnostics`）

只能给出「零裸调用」这一必要条件；权威诊断请由主程用 `lens_diagnostics`
（source=lsp, scope=paths, severity=hint）跑这两个文件确认。

```
$ grep -nE "(^|[^A-Za-z_])printf[[:space:]]*\(" simplexc/cli/tcblob.cc simplexc/lower/lower_selftest.cc
(0 处)
$ grep -c "std::printf" simplexc/cli/tcblob.cc simplexc/lower/lower_selftest.cc
simplexc/cli/tcblob.cc:0
simplexc/lower/lower_selftest.cc:0
```

### P2 编译干净

```
$ python simplexc/tools/build.py tcblob          # 含 objs
== tcblob ==
$ D:/bin/w64devkit/bin\g++.exe -std=c++17 -O2 -static simplexc/cli/tcblob.cc build/objs/assemble.o -o build/tcblob.exe
  -> build/tcblob.exe（2524474 B）
全部完成：objs, tcblob                                   # rc=0，g++ 输出里没有一条 warning

$ D:/bin/w64devkit/bin/g++.exe -std=c++17 -O2 -I simplexc -I build/llvm/include \
    -o work/tmp/printf/lower_selftest_after2.exe \
    simplexc/lower/lower_selftest.cc simplexc/lower/lower.cc simplexc/lower/llvm_ffi.cc \
    simplexc/assemble/assemble.cc -L build/llvm/lib -lLLVM-C
rc=0，stderr 为空（逐字节同样地重编两次 → exe sha256 相同：de0a2670…）
```

额外（比判据更严）：两文件各自 `-Wall -Wextra` 单编 **零告警**：

```
$ g++ -std=c++17 -O2 -Wall -Wextra -c simplexc/cli/tcblob.cc -o …            # rc=0，无输出
$ g++ -std=c++17 -O2 -Wall -Wextra -I simplexc/lower -I simplexc -I build/llvm/include \
      -c simplexc/lower/lower_selftest.cc -o …                               # rc=0，无输出
```

⚠️ `build/tcblob.exe` 体积 552902 B → 2524474 B：静态链 libstdc++ 的 iostream 进来的，不是产物变化。
将来若有人拿体积当「产物没变」的判据，先看这一条。

### P3 `--run` 行为不变

```
$ ./build/tcblob.exe --obj build/objs/entry_shim.o --obj build/objs/rt_min.o \
      --obj build/objs/prog_empty.o --entry __tc_entry --out work/tmp/printf/tcblob_check.bin --run
rc=0
--- 改前 ---
blob: out_len=776 entry=0x0 text=608 rdata=80 data=0 bss=0 stubs=4 relocs=9
写出 work/tmp/printf/tcblob_check.bin（776 字节）
写出 work/tmp/printf/tcblob_check.bin.stubs（4 处 extern 跳板）
RUN ok：blob 在 000001f0630b0000 上跑完，线程退出码 0（0 = 正常 ExitThread）
--- 改后 ---
blob: out_len=776 entry=0x0 text=608 rdata=80 data=0 bss=0 stubs=4 relocs=9
写出 work/tmp/printf/tcblob_check.bin（776 字节）
写出 work/tmp/printf/tcblob_check.bin.stubs（4 处 extern 跳板）
RUN ok：blob 在 0x28ebccc0000 上跑完，线程退出码 0（0 = 正常 ExitThread）
```

`diff` 只有那一行（地址每次运行本来就不同，且 `%p` 形式变了，见下），退出码 0、`RUN ok` 在。
`probe：…` 那行用 `build/llvm_prog.exe` 产出的探针 obj 单独跑过改前/改后两份 tcblob（含 `entry=0x200`
的非零入口）：`diff` 同样只剩 `%p` 那一行，`probe：tc_probe_ok 被调用 1 次，最后一次参数 = 42` 逐字符相同。

### P4 `.entry` / `.stubs` 未被破坏

三者 **sha256 与改前逐个相同**（`--run` 后再算）：

```
445f34371ee9984d1be691c8813d5c325d248d813cab97709e94aea6398e9ba8  tcblob_check.bin
13bf7b3039c63bf5a50491fa3cfd8eb4e699d1ba1436315aef9cbe5711530354  tcblob_check.bin.entry
e362a456fe3207f01e379090aaa7bf1665970ff1a7bd0600d890ea4789222592  tcblob_check.bin.stubs
$ cat tcblob_check.bin.entry
0
$ cat tcblob_check.bin.stubs
offset	kind	symbol
2e0	slot	__imp_ExitThread
2e8	slot	__imp_CreateFileA
2f0	slot	__imp_WriteFile
2f8	slot	__imp_CloseHandle
```

（探针那份 blob 的 `.entry`/`.stubs` 与 HEAD 版 tcblob 跑出来的也逐字节相同；blob 本身在两份 **exe**
之间不同 —— 那是桩里嵌的宿主函数地址 `&tc_probe_ok` 随 exe 变，同一个 exe 连跑两次是逐字节相同的。）

### P5 lower_selftest 输出等价

```
$ PATH="build/llvm/bin:$PATH" ./lower_selftest_before.exe all > …/lower_before.stdout.txt      # rc=0，283 行
$ PATH="build/llvm/bin:$PATH" ./lower_selftest_after2.exe all > …/lower_final.stdout.txt      # rc=0，283 行
$ diff  …/lower_before.stdout.txt …/lower_final.stdout.txt
（空）
$ cmp   …/lower_before.stdout.txt …/lower_final.stdout.txt && echo BYTE-IDENTICAL
BYTE-IDENTICAL
```

`coff` / `ir` / `probe` 三种模式同样逐个逐字节相同（stdout + stderr，退出码 0）。
改前可执行文件是**改动前**当场编的（编完才动源码；`git diff` 显示除本次改动外与 HEAD 无差异）；
tcblob 的改前版另用 `git show HEAD:simplexc/cli/tcblob.cc` 取源码现编（`work/tmp/printf/tcblob_head.exe`）。

### P6 判据入口 `check.py`

```
$ python simplexc/tools/check.py
  步骤             状态      rc       耗时  摘要
  corpus         PASS     0    0.74s  manifest_rows=98 · diff_sites=5553 · unclassified=0 · rebased_identical=98/98 · verdict=校验通过
  host_selftest  PASS     0    0.24s  ticks=3/3 · tick_exact=True · report_passed=True
  lower_equiv    PASS     0    0.23s  cli_rc=0 · blob_bytes=26340 · ours_entry=0x63A0 · ticks=10/10 · diff_count=0 · verdict=逐 tick 全等
  hook_offline   FAIL     1    1.64s  cases=0/8 · build_ok=False · baseline_differs=True · not_run=ours-missing,…
  probe_006      PASS     0    0.48s  ticks=3 · identical=True · semantic_diffs=0
总计 5 步：4 PASS · 1 FAIL · 0 SKIP   用时 12.24s
```

（在**最终文件状态下**复跑过一次，同形：4 PASS + `hook_offline` 同样 FAIL、同样 4 条 undefined reference，
`lower_equiv` 仍 `ticks=10/10 · diff_count=0`。）

`hook_offline` 的 FAIL **与本次改动无关**，证据三条：

1. 报错是 `ld: compile_hook.c:(.text+0x9c1): undefined reference to 'simplexc_free'` /
   `'simplexc_compile'` —— 引用的是**另一个 worker 正在改的** `tools/compile_hook.c` +
   未跟踪的 `simplexc/shell/simplexc_core.cc`；
2. `hook_offline.py` 的编译命令是
   `gcc -shared -O2 -static -o … [src]`，`src` 只有 `tools/compile_hook.c` 与它自造的 HEAD 基线副本，
   `grep -n "tcblob\|lower_selftest" simplexc/tools/hook_offline.py` → **零匹配**；
3. **复现性检验**：把我改的两个文件 `git checkout --` 回 HEAD（即还原成 printf 版）后单跑
   `python simplexc/tools/check.py --only hook_offline` → **同样 FAIL、同样 4 条 undefined reference**；
   随后把我那两份文件（先备份在 `work/tmp/printf/keep/`）拷回，sha256 与备份一致。

## 形式差异（唯一一处）

`tcblob.cc` `--run` 的 `RUN ok…` 行：`%p` → `cout << const void*`。
改前 `000001f0630b0000`（`%p` 在 MinGW 上补零到指针宽度），改后 `0x28ebccc0000`。
这行**只给人看**（`grep -rn "RUN ok" docs/ simplexc/tools/` 只有 `docs/notes/2026-10-17-build-script.md`
把它当输出**引用**过，没有任何脚本解析它）；
按任务书允许的形式差异接受；地址本身每次运行都不同，改动前后都不可当判据。

## 判据入口的偏差（必须知会）

P3 命令里的 `build/objs/blob_empty.o` **不存在**：全仓 grep 不到该名字，
`build.py spike` 用的是 `build/objs/prog_empty.o`（`blob_empty.bin` 就是拿它装的）。
本次 P3 用的是 `prog_empty.o`，其它参数与命令原样一致。

## 产物落盘

`work/tmp/printf/`：`build_before.log` / `build_after.log` / `lower_build_before.log` /
`lower_build_after.log`、`run_before.stdout.txt` / `run_after.stdout.txt` / `run_final.stdout.txt`、
`lower_before.stdout.txt` / `lower_after.stdout.txt` / `lower_final.stdout.txt`、`lower_diff.txt`、
`mode_{coff,ir,probe}_{before,after}.txt`、`probe_{head,new}*.out`、`keep/`（改后源码备份）、
`head/`（`git show HEAD:` 取出的原始源码，用来编改前可执行文件）、`lower_selftest_before.exe` /
`lower_selftest_after{,2}.exe` / `tcblob_head.exe`。

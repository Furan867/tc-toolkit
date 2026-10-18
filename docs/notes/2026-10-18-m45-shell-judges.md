# M4.5 判据器材：core_selftest 的 `.stubs` + hook_offline 的构建与 case 语义（2026-10-18）

**结论**：两处判据器材已与「壳接 C++ 编译器」对齐。`check.py` 5 步全 PASS、
`hook_offline.py` 8/8 通过且 `构建：rc=[0, 0]`、`core_selftest` 产出 `.stubs` 后宿主不再拒跑
（rv32im 10 tick 与参考产物差异 0 条）。

改动三个文件：`simplexc/shell/core_selftest.cc`（加 `.stubs` + 两条路一致断言）、
`simplexc/tools/hook_offline.py`（构建对齐 + case 语义对齐）、
`simplexc/tools/build.py`（**只有两行 docstring**，无行为改动，见文末）。
冻结的 `simplexc_core.{h,cc}`、`tools/compile_hook.c`、`frontend/`、`lower/`、`cli/`、`assemble/` 一律没动。

## ① core_selftest：`.stubs` 从哪来 + 「两条路一致」断言

`simplexc_compile` 的契约（冻结的 C 边界）里没有 stubs，所以 `core_selftest.cc` 里**自己再走一遍**
和 `shell/simplexc_core.cc` 相同的链，只为了拿到 `tc::AsmResult::stubs`：

```
Frontend(SrcText{src}) → lower::build_module(fe, fe.src_path) → lower::Program(ast, false)
  → prog.emit_object() → tc::assemble({module, entry_shim.o, rt_min.o}, "__tc_entry", Kernel32Resolver)
```

后两个目标文件用**构建期嵌进 exe 的字节**（`shell/embedded_objs.hh`，与核心同一份）—— 不是从
`build/objs/` 读文件，于是没有「cwd 必须是仓库根」这种隐性前置条件，装配输入也与核心逐字节同源。

写盘格式与 `cli/simplexc_cli.cc` 的 sidecar **逐字节一致**（`cmp` 通过、md5 相同）：
表头 `offset<TAB>kind<TAB>symbol`，offset 十六进制小写且相对机器码起点，`__imp_` 前缀 → `slot`、其余 → `stub`。
注意 `write_stubs()` 故意**不**开 `std::ios::binary`：CLI 那边是 `fopen(..., "w")`（文本模式，`\n` → CRLF），
两份 sidecar 要能直接比 md5，换行也得一样（第一版忘了这条，185 B vs 192 B）。

**断言（这条本身是判据）**：内部路径的 `AsmResult::blob.bytes` 与 `entry_off` 必须与
`simplexc_compile` 给的产物**逐字节相同**；不同就打印首个不同字节的偏移、退出码 1、**一个文件都不写**
（不是先写一半再报错）。同一份源码、同一批目标文件、同一进程 → 确定性成立（rv32im 实测通过）。
这条断言不成立时，下面那份 `.stubs` 说的可能是另一个产物的偏移，宿主会静默读错内存。

## ② hook_offline 的构建：最终命令行

`new` 变体（**与 `build.py: step_simplexc_dll` 同形**，源文件表直接取 `build.py` 的 `CORE_SOURCES`、
`REL`、`tool`、LLVM 路径 —— 一处定义，没有第二份抄本；`cwd = 仓库根`、参数写相对路径是这条命令的必要条件）：

```
$ python <repo>/simplexc/tools/gen_embedded_objs.py
$ gcc -c -O2 tools/compile_hook.c -o work/tmp/hook_offline/compile_hook_shell.o \
      -DOUT_DIR="…/hook_offline/new/out" -DOPT_DIR="…/new/opt" -DHOOK_LOG_DIR="…/new/opt" \
      -DOPT_INI="…/new/opt.ini" -DOPT_SCRIPT="…/tools/rewrite_stub.py"
$ g++ -shared -O2 -static -o <abs>/work/tmp/hook_offline/bin/compile_hook_new.dll \
      work/tmp/hook_offline/compile_hook_shell.o \
      simplexc/shell/simplexc_core.cc simplexc/shell/embedded_objs.cc <CORE_SOURCES…> \
      -I simplexc -I build/llvm/include -L build/llvm/lib -lLLVM-C
```

`base` 变体（HEAD 源码 + 机械 `#ifndef` 化）仍是 **gcc 单文件编**：HEAD 版不引用 `simplexc_compile`，
不需要 C++ 运行库：

```
$ gcc -shared -O2 -static -o <abs>/…/compile_hook_base.dll <-D 路径宏…> <abs>/…/compile_hook_base.c
```

- **路径宏只给 C 壳那一步**（case 之间的隔离靠它们）；链接那一步不需要。
- **`LLVM-C.dll` 拷进 `bin/`**：ctypes 用 LOAD_WITH_ALTERED_SEARCH_PATH 加载 dll，依赖从**被加载 dll
  自己的目录**开始找 —— `compile_real.dll` 的 `libgcc_s_seh-1.dll` / `libwinpthread-1.dll` 现在就靠这个
  机制从 `bin/` 里被找到，新壳的 LLVM 依赖同理。找不到 `build/llvm/bin/LLVM-C.dll`（或 `build/LLVM-C.dll`）
  时 `build()` 抛 `BuildError` 并写清去哪取。
- `build()` 的 `commands` / `rc` **每个 variant 一项**（`rc=[0,0]`），新壳那项要生成器 + gcc + g++ 三步全 0。
- 前置条件从「`build/blob_empty.bin` 存在」改成「`CORE_SOURCES` 都在 + `libLLVM-C.a` 在」——
  **`build.py spike` 不再是 hook_offline 的前置**。

## ③ case 语义变更逐条

| 原 case | 现状 | 理由 |
|---|---|---|
| `ours-missing`（mode=ours + blob 缺失）| `ours-nosrc`：mode=ours + **空源码** → 壳自己判 `InternalError`：失败串 `ours: InternalError`、`out_len=0`、manifest `impl=ours_failed`、hook.log `error`(stage=ours/kind=InternalError) + `impl`(impl=ours_failed, mode=ours) 两条 | 前提「blob 缺失」随 `OURS_BLOB` 宏一起消失（新壳在进程内编源码，一个 blob 都不读）。失败触发点换成壳自己的 `slen <= 0 → KIND_INTERNAL`，**断言一条没放宽**，另加一条更严的 `out_ptr == 0`（失败 Result 不许留悬垂指针）|
| `fallback-missing` | `fallback-nosrc`：空源码 + ours_with_fallback → 整单转交参考，产物与**同一进程**内 reference 产物逐字节相同、`impl=fallback` | 同上（触发点换空源码）；「转交参考」的链路与断言不变 |
| `ref` | 不变（mode=reference，058.si）| 与 blob 无关 |
| `ours-ok`（产物与「改动前」逐字节相同）| 改为：产物头 **bit62=1**（是我们的缓冲）、`impl=ours`、无 `error`、**同进程两次编译逐字节相同**、**manifest 的 out_len/out_sha256 与实际产物一致** | 旧断言的比对对象是旧壳读 `blob_empty.bin` 得到的产物，**对象已不存在**；换上的都是仍然可判、且更贴「产物确实来自我们的核心而不是悄悄回退」的断言（不是「随便」化）|
| `refcmp`（reference 不回归）| 不变 | 两份壳在 reference 模式下都转发给同一个 `compile_real.dll` 实例，逐字节可判；实测两边 sha256 相同（`f295cdd190d031ed`）|
| `timeout-ours` / `timeout-fallback` | 去掉「blob 64 MB」这个输入（预算 0 = 任何耗时都算超预算），断言不变 | blob 不再是输入，64 MB 只是白写文件；预算 0 的判据语义与原来完全一样 |
| `big-ours-ok`（照 64 MB blob 的对照）| `ours-budget-ok`：**同一份源码** + 默认预算 5 s → ours 成功；断言从「out_len == 64 MB」改成「out_len>0 + impl=ours + 无 error」 | 它作为 `timeout-ours` 的对照要守的是「超时是预算造成的，不是源码编不出来」，与 blob 大小无关 |

case 数与名字变了（8 → 8，两个改名）：`ours-missing` → `ours-nosrc`、`fallback-missing` → `fallback-nosrc`、
`big-ours-ok` → `ours-budget-ok`。名字必须能读出它测什么，否则下一个人会把「缺失」当成还是 blob 的事。

## ④ 判据 C1–C6（命令 + 输出）

**C1 构建**
```
$ python simplexc/tools/build.py simplexc_dll core_selftest        → rc=0
  -> build/compile_hook.dll 3396424 B / build/core_selftest.exe 3384229 B
  日志里 "warning" 计数 = 0
$ objdump -p build/compile_hook.dll | grep "DLL Name"
  LLVM-C.dll / KERNEL32.dll / msvcrt.dll            （无 python）
Export Address Table 00000003                        （导出仍是 3 个）
$ g++ -std=c++17 -O2 -Wall -Wextra -c simplexc/shell/core_selftest.cc -I simplexc -I build/llvm/include -o work/tmp/shell/core_selftest_we.o
  WEX_rc=0（零告警）
```

**C2 产物 + `.stubs` + 宿主**
```
$ ./build/core_selftest.exe --src simplexc/corpus/rv32im/rv32im.si --out work/tmp/shell/c2.bin
  out_len=26340 entry=0x63A0 头 bit62=1 桩数=6 → work/tmp/shell/c2.bin
$ head -3 work/tmp/shell/c2.bin.stubs
  offset<TAB>kind<TAB>symbol / 66a0<TAB>stub<TAB>Sleep / 66b0<TAB>stub<TAB>GetSystemTimePreciseAsFileTime
  （7 行 = 表头 + 6 条：2 桩 + 4 槽）
$ cmp c2.bin c2_cli.bin          → 逐字节相同（md5 a2134745758a14902665f385a80c0db0）
$ cmp c2.bin.stubs c2_cli.bin.stubs → 逐字节相同（md5 8bbd4895c02c630308fc39c39b3a43da）
$ python simplexc/tools/host/host.py --src simplexc/corpus/rv32im/rv32im.si \
      --bin work/tmp/shell/c2.bin --entry 0x63A0 --ticks 10 --seed-reset-data seq --out …/c2_ours.jsonl
  rc=0；"硬错误" 计数 0；extern 换桩 6 个（2 桩 + 4 槽）；tick 1..10 全 ok
$ 参考产物：… --bin simplexc/corpus/rv32im/rv32im.bin --entry 0x215B …（rc=0）
$ selftest.compare(ref, ours) → ticks=10/10 diff_count=0
```

**C3 壳的离线判据**
```
$ python simplexc/tools/hook_offline.py --json work/tmp/shell/c3_hook_offline.json   → rc=0
  8 个 case：8 通过 · 0 失败 · 0 未跑（构建/编译失败）  用时 20.38s
  构建：rc=[0, 0]（18.931s）        （全日志 43 条 [OK]、0 条 [FAIL]）
  refcmp: sha256_new=f295cdd190d031ed / sha256_base=f295cdd190d031ed
```

**C4 五步判据**
```
$ python simplexc/tools/check.py --json work/tmp/shell/c4_check.json   → rc=0
  corpus PASS · host_selftest PASS · lower_equiv PASS · hook_offline PASS · probe_006 PASS
  总计 5 步：5 PASS · 0 FAIL · 0 SKIP   用时 30.33s
```

**C5 诊断**：`lens_diagnostics`（source=lsp / severity=hint）**不在我这一轮的工具白名单里**，没跑成 ——
这条要由验收方跑。替代证据：改动工具的 C/C++ 与 Python 内建诊断报告 0 问题（它抓出过真问题：
`importlib.util imported but unused`）、`-Wall -Wextra` 编 `core_selftest.cc` 零告警、
`python -m py_compile hook_offline.py` 通过。需要跑的文件：`simplexc/shell/core_selftest.cc`、
`simplexc/tools/hook_offline.py`、`simplexc/tools/build.py`。

**C6 规范**：新代码没有 printf 家族（`core_selftest.cc` 走 `std::cout` + **局部** `std::ostringstream`
拿十六进制）；没有 `noqa` / `NOLINT` / 改配置；没有 UB（`memcmp` 的长度就是被断言的那一个 `len`、
所有窄化都写 `static_cast`、`std::hex` 只落在局部流上）。

## ⑤ 对 `baseline_differs` 的决定

**决定**：保持「**只记录、不作为通过条件**」，并在 docstring 里写清为什么现在 `True` 是预期的。

论证：
1. 它**本来就不是**通过条件 —— 没有任何 case 读它（只是 `note_baseline()` 写进 facts、
   `check.py` 摘要里带一下、`main()` 在 False 时打一行告警）。所以这里不需要「放宽」，只需要把
   语义写清。
2. 新版壳接了真编译器，两份 dll 的**构建命令形状**都不同了（base = HEAD 源码 + gcc 单文件；
   new = 当前源码 + `gcc -c` 后 `g++` 链核心 + LLVM），源文本必然不同 → `True`。
   它已经从「工作区脏不脏」变成了「两份壳是不是同一个东西」，继续要求 False 是没有意义的。
3. `refcmp` 判的东西与它无关：两份壳在 reference 模式下都把整单调给同一个 `compile_real.dll` 实例，
   产物逐字节相同（实测两边 sha256 都是 `f295cdd190d031ed`）—— 这才是「reference 不回归」的证据。
4. 顺带把两处随它而来的文字改了：docstring 的「`refcmp` / `ours-ok` 是自比」→「`refcmp` 是自比」
   （ours-ok 不再比基线），`main()` 的告警同改；并让 `main()` 在构建成功时也打印
   `构建：rc=[…]（…s）`（原来是只在失败时打印，验收要看的就是这一行）。

## 附带：两处需要验收方知道的事

1. **`build.py` 只有两行 docstring 同步**（无行为改动）：`core_selftest` 目标的说明补上 `.entry` / `.stubs`；
   `spike` 目标里「现在只有旧的基线壳还会读它（离线判据的 base 变体）」已经不成立 ——
   改成「现在没有判据读它：新版壳在进程内编源码，基线壳只在 reference 模式下被驱动」。
   `build.py` 这次**没有**其它改动：hook_offline 是 `importlib` 按路径加载它、直接复用
   `CORE_SOURCES` / `REL` / `tool` / LLVM 路径常量，所以不需要抽新常量。
2. **冻结的 `simplexc/shell/simplexc_core.h` 在我这一轮里被另一个写者改过**（mtime 10:26:08，
   与 `simplexc_core.cc` 同一秒）：从「纯 C 头、无 `#ifdef __cplusplus` 守卫」变成了标准的
   `#ifdef __cplusplus extern "C" {}` 守卫，`simplexc_core.cc` 的 include 也从 `extern "C" { #include }`
   改成裸 include。我按禁令没碰这两个文件，我的 `core_selftest.cc` 仍用 `extern "C" { #include }` 包住它
   （对新旧两种头都成立）。C1–C4 的证据全部是在**改动之后**的字节上重编重跑出来的（10:31–10:45）。
   已向主程汇报。

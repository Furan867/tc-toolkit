# build.py：从既有产物反推构建命令（2026-10-17）

## 结论

`simplexc/tools/build.py` 能一键重建 `build/` 下的全部产物，且**与既有产物逐字节相同**
（源码版本一致时）。7 个 README（`simplexc/` + 六个子目录）一并补上。

反推方法：把候选命令编出来的目标文件/可执行文件与 `build/` 里的既有产物**比 md5**，
不一致就换 flag —— 不是读代码猜。证据命令与结果见下。

## 反推出来的命令（各步的判据 = 与既有产物 md5 相同）

| 产物 | 命令 | 判据 |
|---|---|---|
| `build/objs/assemble.o` | `g++ -std=c++17 -O2 -c simplexc/assemble/assemble.cc -o …` | md5 `f9195203…` = 既有产物（用 HEAD 版源码；工作区源码已改，见「存疑」） |
| `build/objs/entry_shim.o` | `g++ -c simplexc/shell/entry_shim.S -o …`（.S 不加 `-std`） | md5 `14269e32…` 相同 |
| `build/objs/rt_min.o` | `g++ -std=c++17 -O2 -c simplexc/runtime/rt_min.cc -o …` | md5 `0743fa86…` 相同 |
| `build/objs/prog_empty.o` | `g++ -std=c++17 -O2 -c simplexc/cli/prog_empty.cc -o …` | md5 `183053ef…` 相同 |
| `build/tcblob.exe` | `g++ -std=c++17 -O2 -static simplexc/cli/tcblob.cc build/objs/assemble.o -o …` | md5 `8d118d49…` 相同 |
| `build/llvm_prog.exe` | `g++ -std=c++17 -O2 -static -I build/llvm/include simplexc/cli/llvm_prog.cc -o … -L build/llvm/lib -lLLVM-C` | md5 `3f41a66a…` 相同 |
| `build/compile_hook.dll` | `gcc -shared -O2 -static -o <abs>\build\compile_hook.dll tools/compile_hook.c` | 用 HEAD 版源码得到 md5 `370a1e95…` = 12:07 的备份 |
| `build/blob_empty.bin` | `build/tcblob.exe --obj rt_min.o --obj entry_shim.o --obj prog_empty.o --entry __tc_entry --out …` | sha256 `3b378baa…` = M1 记录值 |

排除的候选（都试过、与既有产物不符）：`-O0/-O1/-O3`、`-O2 -fno-exceptions`
（既有 assemble.o 里有 `__gxx_personality_seh0` / `_Unwind_Resume` → 异常是开着的）。
`-static` 在 w64devkit 上不改变字节（加不加逐字节相同），显式写着是为了换工具链时不悄悄多出 DLL 依赖。

`build/objs/llvm_prog.o` **不是编译产物**：它是 `build/llvm_prog.exe out.obj` 的输出（LLVM 生成的 COFF，
374 B，只有 `tc_main` 与未定义的 `tc_probe_ok`）。别按名字当成 `llvm_prog.cc` 的目标文件。

## 三条路径敏感性（都是实测，写进 build.py 的硬要求）

1. **`-L` 的写法会改变产物字节**：`-L ../../../../build/llvm/lib`（相对）与
   `-L D:/…/build/llvm/lib`（绝对）链的是同一个 `libLLVM-C.a`、link map 逐行相同（同样的成员/顺序），
   但产物 md5 不同（`3f41a66a…` vs `c3d7ce41…`）：差的是 `.text` 里的 import thunk 表布局。
   所以 build.py 的 cwd = 仓库根、参数一律相对写法。机制没查清（见「存疑」）。
2. **DLL 的 `-o` 写法决定 image base**：ld 给 DLL 挑的默认基址随输出路径字符串变
   （同一路径两次编译逐字节相同；换个目录/名字就换基址，产物差 ~150 字节）。build.py 与
   `tools/deploy_compile_hook.py` 用同一种拼法 → 两者产出的 DLL 逐字节相同。
3. **源码的 basename 会进产物**：同一份 HEAD 源码，编到 `head_compile_hook.c` 得 107842 B /
   `1422e9cb…`，编到 `compile_hook.c` 得 107822 B / `370a1e95…`（= 备份）。差 20 字节 = basename
   在 4 处 `.file` 符号里（15 → 20 字符 × 4）。所以「改个文件名再比 md5」比不出结论。

## 验收判据（实跑，2026-10-17 14:22–14:26）

```
$ rm -rf build/objs build/tcblob.exe build/llvm_prog.exe build/compile_hook.dll \
        build/blob_empty.bin build/blob_empty.bin.entry
$ python simplexc/tools/build.py all spike        # 窗口 ~40 s
rc=0；objs 4 个、tcblob.exe、llvm_prog.exe、compile_hook.dll、blob_empty.bin(+.entry) 齐全
  -> build/blob_empty.bin（728 B）  sha256 3b378baa5c12f287c8b3d46d42be186e3d5d4c58fdffb325839612ff2c777472

$ build/llvm_prog.exe <out.obj>                  # 重建版与备份版
both rc=0，输出「COFF 374 字节 → <out>」，产出的 .obj md5 两边都是 228f319301ccd6ca426037883e7188cd
build/llvm/probe.exe（本次没重建）→ 「RESULT: OK —— LLVM-C 链上了，拿到 COFF 目标码」

$ build/tcblob.exe --obj build/objs/rt_min.o --obj build/objs/entry_shim.o \
      --obj build/objs/prog_empty.o --entry __tc_entry --out /dev/null --run
blob: out_len=728 entry=0x200 text=608 rdata=80 data=0 bss=0 stubs=4 relocs=9
RUN ok：blob 在 00000187d8ed0000 上跑完，线程退出码 0（0 = 正常 ExitThread）  rc=0

$ python simplexc/tools/host/selftest.py --src simplexc/corpus/src/006.si \
      --bin simplexc/corpus/ref/006.bin --ticks 3
自比对 通过：tick 3/3（要 3）· 逐 tick 全等=是 · 仿真真在工作=是（工作页 1 个）· extern 0 条   rc=0

$ python simplexc/tools/recompile_corpus.py --check
校验通过：manifest、corpus/src、corpus/ref 与捕获三者一致，同源检查三条断言全过   rc=0

$ python simplexc/tools/build.py all spike   # 连着跑第二遍
md5sum -c 全 OK → 幂等（逐字节相同）
```

`build/compile_hook.dll` 与 `tools/deploy_compile_hook.py build` 的产物逐字节相同
（两边 md5 都是 `ee4bbe7a…`，源码 md5 `8d6d4532…`）。

备份在 `work/tmp/build_backup/`（objs/ + 三个 exe/dll + blob + blob_llvm.bin），
比对的中间产物在 `work/tmp/flagcheck/`。

## 存疑 / 未解决

- **备份与当前工作区源码的版本差**：`tools/compile_hook.c` 在 14:21 被另一个 agent 改过
  （`impl` 行 per-call 那一批，14:26 又改了一次，md5 从 `8d6d4532` → `910e9b49`），
  `simplexc/assemble/{assemble.cc,assemble.hh}` 在 14:20 也被改过
  （`BadObject` → `TooLarge`）。所以 12:07 的 `compile_hook.dll` 备份、12:17 的 `assemble.o`/`tcblob.exe`
  备份都与**当前源码**的产物不同 —— 这是源码变了，不是构建命令不对：拿 HEAD 版源码 + 同一条命令，
  两个产物都能逐字节重现备份（`370a1e95…` / `f9195203…`）。当前 `build/` 里留的是**当前工作区源码**
  编出来的产物（`compile_hook.dll` = `7ff5a8a1…`，`assemble.o` 随工作区改动变），与 build.py 的语义一致。
  因为 `compile_hook.c` 有并行写者，`compile_hook.dll` 的 md5 会随他每次改动而变 —— 判据不是「等于备份」，
  而是「等于 `tools/deploy_compile_hook.py build` 的产物」（每次实测逐字节相同，这才是同一条命令）。
- **ld 为什么随 `-L` 写法改产物**没查清：输入相同、成员相同、map 相同，只有字节不同。
  现在的做法（相对写法）能稳定重现，但不是从机制上理解的 —— 换 binutils 版本时要重验。
- M1 的 sha256 判据只覆盖 `blob_empty.bin`（空程序）。真正电路的产物没有可比的既有版本 ——
  要等前端（M3）产出第一份真程序，才能验证「字节可复现」在复杂产物上还成立。
- `build/llvm_prog.o` 与 `build/blob_llvm.bin` 是 spike 的手工残留，build.py 不生成、也不删它们。

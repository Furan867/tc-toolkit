## 构建与 LLVM

### LLVM 固定版 = 官方 23.1.3（win64 / MSVC 构建）+ 自造 MinGW 导入库
资产 `clang+llvm-23.1.3-x86_64-pc-windows-msvc.tar.zst`（263.5 MB），
sha256 `7e69dc07a4536b79ff24a651a2611990bf8eaab17ea560f1092707c4f31ba01d`
（GitHub API 的 `digest` 与本地重算一致）。
取件：`python tools/fetch_llvm.py --dest D:/downloads` → `build/llvm/{bin/LLVM-C.dll, include/**, lib/LLVM-C.lib}`
（不进 git；`build/llvm/llvm.json` 记版本/资产名/sha256）。MinGW 导入库：
`gendef LLVM-C.dll`（1240 个导出）→ `dlltool -m i386:x86-64 -d LLVM-C.def -l lib/libLLVM-C.a`
（MSVC 那份 `LLVM-C.lib` 没用上）。运行期靠系统中已有的 MSVC 运行时
（`vcruntime140.dll` / `vcruntime140_1.dll` / `msvcp140.dll`）。
验证：`g++ -std=c++17 -O2 -I build/llvm/include simplexc/shell/llvm_link_probe.cc -o build/llvm/probe.exe -L build/llvm/lib -lLLVM-C`
      编译通过，运行输出 `RESULT: OK`
最后验证：2026-10-16

同一个包里解出的 `bin/clang.exe`（M1.5 的产物必须由 clang 生成）：
`python tools/fetch_llvm.py --offline --tag llvmorg-23.1.3 --extra bin/clang.exe`，sha256 `9584244d83bb…`。
`--offline` 用本地压缩包 + `llvm.json` 里记的 sha256 重解（GitHub 临时 API 会 403）；
顺手修了旧实现的一个 404：按 tag 查要走 `releases/tags/<tag>`（`releases/{ref}` 只对 `latest` 合法）。
验证：`build/llvm/bin/clang.exe --version` → `clang version 23.1.3 (…0d261d1ca552…)`
最后验证：2026-10-17

### LLVM 产出的 COFF：装配器的输入形状
`x86_64-pc-windows-gnu` + `RelocMode=Static` + `CodeModel=Small` + `default<O2>`：
- 节：`.text` / `.data` / `.bss`（空的也在）；有栈帧的函数额外带 `.pdata` / `.xdata`；
- `.text` 重定位**只有** `IMAGE_REL_AMD64_REL32`，目标有三类：内部函数、未定义 extern、
  **`___chkstk_ms`**（>4 KiB 栈帧才出现，是 CRT 符号不是 kernel32）；
- `.pdata` 用 `IMAGE_REL_AMD64_ADDR32NB`（指向 `.text`/`.xdata`）→ **整节丢弃**即可，别去解；
- 未定义符号在 COFF 里是 `sec 0` + `scl 2`。
验证：跑 `build/llvm/probe.exe build/llvm/probe_obj.obj` 后 `objdump -h/-t/-r build/llvm/probe_obj.obj`
最后验证：2026-10-16

### COFF 的 REL32 addend 是**有符号**的
`IMAGE_REL_AMD64_REL32` 的字段内容按有符号 32 位参与 `S + A - (P + 4)`；GCC 会用 `A = -1` 表示
「字面量地址减一」的偏置（它的逐字节复制循环从 `[r8 + i + 1]` 读回）。当无符号算，每处这类引用都会
溢出 ±2GB。
验证：`simplexc/assemble/assemble.cc` 里的 `int32_t addend = int32_t(r32(...))`；
      `build/objs/rt_min.o` 的 `.text+0x27`（字段 `FF FF FF FF`，对 `.rdata`）与 w64devkit `ld` 链接后的位移一致
最后验证：2026-10-17

### 链接产物随 `-L` 写法与 `-o` 路径变化
`-L` 写绝对路径与写相对路径时 link map 逐行相同、产物字节**不同**（import thunk 表布局变了）；
DLL 的 `-o` 换成别的路径字符串会换掉 ld 挑的默认 image base（产物差 ~150 字节）。
`simplexc/tools/build.py` 因此固定「相对 `-L` + 与 `deploy_compile_hook.py` 同一种绝对 `-o` 拼法」。
验证：`work/tmp/flagcheck/` 里两份 link map 与 md5 清单；`build.py hook` 与 `deploy_compile_hook.py build` 产物逐字节相同
最后验证：2026-10-17

### 产物必须是能驱动仿真的程序：空/桩程序会让游戏在**下一次**编译请求时 abort
游戏每条编译请求都是先 `send_command(3)` + `joinThreads()` 再 `compile()`；而一个什么都不做就
`ExitThread` 的产物会让仿真协议处于半途，下一次编译请求就在游戏自己的状态机里 fatal 退出。
特征（用于与真崩溃区分）：**不产生 WER 报告、不产生 CrashDump**，且因死在 `compile()` 之前，
`work/hook/` 里连 `hook_src_NNN.si` 都不会新增。
验证：2026-10-17 游戏内实测（`compiler = ours` 返回 728 字节空程序 → 下一次手动编辑触发编译时 abort；
      `%LOCALAPPDATA%\CrashDumps` 与 WER ReportArchive 无新记录，`manifest.tsv` 停在 seq 006）
最后验证：2026-10-17

## 装配器与产物确定性

### 产物逐字节可复现的前提：不读「没有原始数据」的节
COFF 对象文件里 `.bss` 是 `SizeOfRawData=N / PointerToRawData=0` —— 文件里没有它的数据（内容隐含全 0）。
装配器若按 rawptr 读 rawsize 字节，读到的是**文件开头的 COFF 头**（Machine + NumberOfSections + TimeDateStamp）
→ 产物带编译时间戳（同一源码两次编译的 blob 不同）且 `.bss` 初值是垃圾。
修法：`raw_ptr==0` 的节留空（payload 本来就零初始化）；节大小 = `max(SizeOfRawData, VirtualSize)`。
⚠️ 对象文件里 `VirtualSize` 实测恒为 0（clang 与 gcc），大小在 `SizeOfRawData`。
验证：同一份 `simplexc/probe/006_equiv.c` 用 clang 连编两次（两个 `.obj` 只差 header 时间戳 1 字节）
      → `build/tcblob.exe` 装配出的两个 blob 逐字节相同；blob 内搜输入 `.obj` 的 COFF 头/时间戳 0 命中
      （修复前的对照与脚本见 `docs/notes/2026-10-17-assemble-bss.md`）
最后验证：2026-10-17

### `.obj` 本身不可复现（clang 每次写 COFF TimeDateStamp），`SOURCE_DATE_EPOCH` 无效
所以「产物确定」只能在**装配后的 blob** 上说；比 sha / 做 A/B 记录都要拿 blob，不要拿 `.obj`。
验证：连编两次 `cmp -l` 只差 offset 4 的 1 字节；设 `SOURCE_DATE_EPOCH=0` 两次仍是不同时间戳
最后验证：2026-10-17

### blob 的 raw sha 只在同一次开机内跨进程可比
系统 DLL 基址一次开机内跨进程共用（实测三个独立进程拿到同一个 `kernel32.base`），所以同一份 `.si`
连编 3 次（3 个进程）raw 逐字节相同；**跨 boot 会变**，差异全部落在 extern 槽。
所以「重构前后产物相同」这类回归要比**掩掉槽位**的归一化 sha，不能比 raw：槽位从 `.stubs` sidecar 取
（`kind=stub` 的 imm64、`kind=slot` 的 8 字节）。
验证：`python simplexc/tools/blob_baseline.py --verify simplexc/tools/baseline` —— 比 6 个字段
（status / blob_len / entry / stub_count / norm_sha256 / stubs_sha256），`raw_sha256` 只记不比；
反向对照：改 1 个非槽字节 → 恰好命中 1 份并报出字节区间，改槽内字节 → 仍判等。
最后验证：2026-10-24

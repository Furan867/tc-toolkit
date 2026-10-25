#!python3
"""simplexc 的一键构建：把 build/ 下的产物全部从源码重建出来。

为什么必须有这个脚本：build/ 不进 git，而 tcblob.exe / llvm_prog.exe / compile_hook.dll
原先都是手敲命令编出来的 —— 换机器就只能靠猜。每条命令都是从既有产物**反推确认**过的
（判据见 docs/notes/2026-10-17-build-script.md：重建出的目标文件与既有产物逐字节相同）。

用法
----
    python simplexc/tools/build.py                # = all
    python simplexc/tools/build.py all            # objs + tcblob + llvm_prog + hook
    python simplexc/tools/build.py objs tcblob    # 只做这几步
    python simplexc/tools/build.py spike          # 空程序 blob（依赖 objs + tcblob，自动补做）
    python simplexc/tools/build.py difftrace      # 判据工具（要 difftrace C SDK，见下）
    python simplexc/tools/build.py difftrace_selftest   # 编出来当场跑 16 条用例
    python simplexc/tools/build.py asm_x64_selftest     # 编出来当场跑 asm_x64 的用例
    python simplexc/tools/build.py --jobs 1             # 串行（= 加并行之前的行为，排查用）

并行
----
默认就并发。能并的只有**目标之间**：每个 step_* 是「拼几条 g++ 命令 + 跑它」，彼此独立；
单个 .cc 内部**没有**可切分的单位（g++ 的一个进程只编一个翻译单元），所以在某个目标内部
加线程毫无用处。也因此**编译命令一个字都没改**（优化等级、-I、宏、顺序敏感的 -l 与 -o 全是原样），
改的只是它们在什么时候跑 —— 产物仍与串行逐字节相同。

`--jobs N` 定并发数（默认 = CPU 核数，上限见 MAX_JOBS；`--jobs 1` = 串行，走原来的顺序循环）。
每个目标的输出（含子进程的 stdout/stderr）各自整块收着，全部跑完再按命令行顺序倒出来 ——
并发时输出不交织才看得出是哪个目标失败。任一目标失败**不打断**其它已启动的（否则日志半截），
跑完汇总，退出码取第一个失败目标的。
有先后关系的才排队（DEPENDS + SCHEDULE_DEPS），其余同时起。

目标
----
    objs       build/objs/{assemble,entry_shim,rt_min,prog_empty}.o
    tcblob     build/tcblob.exe        装配器 CLI（--run 可不进游戏自测一次产物）
    llvm_prog  build/llvm_prog.exe     LLVM-C 生成程序根（M1 那条链的样本）
    hook       build/compile_hook.dll  转发层（C 壳 + C++ 核心；= simplexc_dll）
    simplexc_dll  同上（与其它 simplexc 目标对齐的名字；部署入口转调它）
    core_selftest build/core_selftest.exe  判据入口：源码字节 → `simplexc_compile` → 产物 +
               `.entry` / `.stubs`（不进 DLL）
    hir_selftest build/hir_selftest.exe  sema 的判据入口：HIR dump 与 `sema/testdata/` 里的
               golden 逐字节比 + 六条不变量（不进产品路径，不依赖 LLVM）
    analyze_selftest build/analyze_selftest.exe  sema 的另一个判据入口：`.si` → 前端 AST →
               `sema::analyze` → dump，与 `sema/testdata/cases/` 的 golden 逐字节比 + 六条不变量
               （不进产品路径，不依赖 LLVM）
    ast_dump   build/ast_dump.exe     前端的判据入口（frontend/ 的 C++17 实现，不进产品 DLL）
    codegen_selftest build/codegen_selftest.exe  codegen 的 L4 判据入口：`.si` → IR，逐条查
               `codegen/testdata/cases/*.irkeys` 的期望子串（不判产物；要链 LLVM）
    spike      build/blob_empty.bin (+ .entry)  「空程序」blob —— 只证明链路通，
               不能拿进游戏（会让游戏在下一次编译请求时 abort，见 docs/pitfalls.md）
               现在**没有判据**读它：新版壳在进程内编源码，基线壳只在 reference 模式下被驱动。
    difftrace  build/difftrace.exe     Unicorn 上的访存序列差分（判据工具，不是产线）；
               顺手把 unicorn.dll / capstone.dll 放到 build/ 旁边。**不在 `all` 里**
    difftrace_selftest build/difftrace_selftest.exe  编出来当场跑，透传退出码；
               只编 diff.cc，**不用** SDK。**不在 `all` 里**
    asm_x64_selftest build/asm_x64_selftest.exe  stdlib `asm x86_64` 块编码器的判据：
               白名单助记符 → 写死的字节（标 `ref` 的取自参考产物）。**不在 `all` 里**

环境变量
--------
    W64DEVKIT_BIN   工具链目录，默认 D:/bin/w64devkit/bin
    W64DEVKIT_CXX / W64DEVKIT_CC   直接指定 g++ / gcc（优先于 W64DEVKIT_BIN 里的）

前置：LLVM（build/llvm/，不进 git）
----
    llvm_prog 需要 build/llvm/include（头）与 build/llvm/lib/libLLVM-C.a（MinGW 导入库）。
    没有就先取：python tools/fetch_llvm.py --dest D:/downloads（见 docs/facts.md §构建与 LLVM）。

前置：difftrace 的 C SDK（work/tools/difftrace-deps/，不进 git）
----
    difftrace 要 Unicorn 2.1.4 与 Capstone 5 的头 + 导入库。
    没有就先跑：python simplexc/tools/difftrace/fetch_deps.py（本脚本不去下载）。
    这也是它**不在 `all` 里**的原因：`all` 的成功不能依赖这份 SDK —— 缺 SDK 的机器上
    照样能 `build.py`，只是不能 `build.py difftrace`。

两条必须遵守的写法（都不是风格偏好，是实测出来的）
--------------------------------------------------
  1. 命令的 cwd 一律 = 仓库根，参数一律写相对路径 —— **ld 的输出会随 -L 的写法变**：
     绝对路径 -L 与相对路径 -L 的 link map 逐行相同（同一个 libLLVM-C.a、同样的成员顺序），
     产物字节却不同（.text 里的 import thunk 表布局）。只有相对写法与 build/ 里既有的产物逐字节相同。
  2. hook 的 `-o` 写成**绝对**路径，且与 tools/deploy_compile_hook.py 用同一种拼法：
     ld 给 DLL 挑的默认 image base 随输出路径字符串变（同一路径两次编译逐字节相同，
     换个路径就换基址，产物差 ~150 字节），写同一种拼法才能与既有 build/compile_hook.dll 逐字节相同。

幂等：每次都重编，不做「产物比源码新就跳过」的判断 —— 命令便宜且确定，跳过判断只会带来
「改了源码但产物是旧的」这种难查的错。不碰 git。
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import threading
import time
import traceback
from concurrent.futures import FIRST_COMPLETED, Future, ThreadPoolExecutor, wait

HERE = os.path.dirname(os.path.abspath(__file__))
SIMPLEXC = os.path.dirname(HERE)
REPO = os.path.dirname(SIMPLEXC)                       # reversing/

BUILD = os.path.join(REPO, "build")
OBJDIR = os.path.join(BUILD, "objs")
# LLVM-C.dll + 头 + 自造 MinGW 导入库（不进 git，见 docs/facts.md §构建与 LLVM）
LLVM_INC = os.path.join(BUILD, "llvm", "include")
LLVM_LIB = os.path.join(BUILD, "llvm", "lib")
LLVM_BIN = os.path.join(BUILD, "llvm", "bin")
# difftrace 的源码目录与它的 C SDK（SDK 不进 git，由 fetch_deps.py 备齐）
DIFFTRACE_DIR = os.path.join(SIMPLEXC, "tools", "difftrace")
DIFFTRACE_DEPS = os.path.join(REPO, "work", "tools", "difftrace-deps")
DIFFTRACE_DEPS_HINT = ("difftrace 的 C SDK（Unicorn / Capstone）不进 git，先跑 "
                       "python simplexc/tools/difftrace/fetch_deps.py 备齐头文件与导入库。")

# 产品核心的源文件表：DLL（`simplexc_dll`）与判据入口（`core_selftest`）**共用这一处**。
# 两者用同一份清单不是风格问题：判据要判的就是交付物 —— 表不一样时判出来的可能是别的东西。
# （`assemble/assemble.cc` 走 `objs` 目标出的 build/objs/assemble.o，不在这里重复编。）
CORE_SOURCES = [
    "simplexc/frontend/decls.cc",
    "simplexc/frontend/lexer.cc",
    "simplexc/frontend/parser.cc",
    "simplexc/frontend/frontend.cc",
    "simplexc/frontend/ast_dump.cc",
    "simplexc/frontend/json.cc",
    "simplexc/codegen/module_build.cc",
    "simplexc/codegen/codegen.cc",
    "simplexc/codegen/asm_x64.cc",
    "simplexc/codegen/llvm_ffi.cc",
    "simplexc/sema/analyze.cc",
    "simplexc/sema/layout.cc",
    "simplexc/sema/types.cc",
    "build/objs/assemble.o",
]

def REL(p: str) -> str:
    """仓库内的路径 → 命令行里的相对写法（正斜杠）。见模块 docstring 的「两条写法」。"""
    return os.path.relpath(p, REPO).replace("\\", "/")


def tool(name: str, env_override: str) -> str:
    """工具链可执行文件的路径：<env_override> → W64DEVKIT_BIN → 默认 D:/bin/w64devkit/bin。"""
    direct = os.environ.get(env_override)
    if direct:
        return direct
    bindir = os.environ.get("W64DEVKIT_BIN", r"D:/bin/w64devkit/bin")
    return os.path.join(bindir, name)


CXX = ""        # 在 main 里填（--help 不该要求工具链存在）
CC = ""

# 并发上限。与 simplexc/tools/m5/batch.py 的 MAX_JOBS 同值（那一条是 difftrace 实测：16 路只比
# 12 路快 7%、多占 1.75 GB）；g++ 这边没单独测过，取同值的理由只是同一台机器、同一类
# 「每进程几百 MB、再多只抢内存」的活。目标数本身就不多（`all` 是 8 个），上限很少吃亏。
MAX_JOBS = 12


class Sink:
    """一个目标的输出缓冲：并发时每目标一份，跑完整块倒出来。

    只存字节、只按追加顺序存（流也记在里面）：文本层与字节层各写各的会把顺序弄乱，
    而「输出不交织」正是并发时唯一还看得出哪个目标失败的依据。
    """

    def __init__(self) -> None:
        self.chunks: list[tuple[str, bytes]] = []      # ("out" | "err", 字节)
        self.rc = 0
        self.seconds = 0.0

    def add(self, which: str, data: bytes) -> None:
        self.chunks.append((which, data))


_TLS = threading.local()        # 当前线程正在跑哪个目标的缓冲（主线程没有 = 串行/编排输出）


def _sink() -> Sink | None:
    return getattr(_TLS, "sink", None)


class Capture:
    """并发时顶替 sys.stdout / sys.stderr：把**当前线程**的写入收进它那个目标的缓冲。

    为什么不是让每个 print 自己找流：串行与并发必须跑同一段 step_* 代码，否则两种模式的
    输出会分叉，`--jobs 1` 也当不成参照物 —— 差别只该在「写去哪」。
    """

    def __init__(self, real, which: str) -> None:
        self._real = real
        self._which = which

    def write(self, s: str) -> int:
        sink = _sink()
        if sink is None:
            return self._real.write(s)
        # 串行时 print 走文本层，"\n" 会被换成 os.linesep；这里直写字节，得自己换，
        # 否则并发模式的换行与串行不一致（子进程写出来的那份已经是 CRLF）。
        text = s.replace("\n", os.linesep)
        sink.add(self._which, text.encode(getattr(self._real, "encoding", None) or "utf-8", "replace"))
        return len(s)

    def flush(self) -> None:
        if _sink() is None:
            self._real.flush()

    def writable(self) -> bool:
        return True

    def isatty(self) -> bool:
        return False


def _emit(which: str, data: bytes) -> None:
    """整块印一段字节 —— 走 buffer，别把子进程的字节按 locale 解码再编回去：
    子进程写的是 UTF-8，而这台机器的 locale 是 cp936，中间那趟会把报错原文弄坏。
    """
    stream = sys.stdout if which == "out" else sys.stderr
    buf = getattr(stream, "buffer", None)
    if buf is None:
        stream.write(data.decode(getattr(stream, "encoding", None) or "utf-8", "replace"))
        stream.flush()
        return
    stream.flush()          # 文本层可能还压着东西，先让它落地，顺序才对
    buf.write(data)
    buf.flush()


def _flush_sink(sink: Sink) -> None:
    for which, data in sink.chunks:
        _emit(which, data)


def run_rc(cmd: list[str]) -> int:
    """跑一条子进程并返回退出码。

    串行时直连终端（子进程继承 fd，行为与加并行之前一致）；并发时把 stdout/stderr 整块收进
    本目标的缓冲 —— 否则几路 g++ 的报错会交织在一起，报错时看不清是哪个目标。
    """
    sink = _sink()
    if sink is None:
        return subprocess.run(cmd, cwd=REPO).returncode
    proc = subprocess.run(cmd, cwd=REPO, capture_output=True)
    if proc.stdout:
        sink.add("out", proc.stdout)
    if proc.stderr:
        sink.add("err", proc.stderr)
    return proc.returncode


def run(cmd: list[str], out: str | None = None, hint: str = "") -> None:
    """跑一条命令；非 0 立即退出（附上这条命令的可执行解释）。"""
    print("$ " + " ".join(cmd))
    rc = run_rc(cmd)
    if rc != 0:
        print(f"FAIL 命令返回 {rc}：{' '.join(cmd)}", file=sys.stderr)
        if hint:
            print("  " + hint, file=sys.stderr)
        print("  整条命令可以原样复制到仓库根复现；工具链路径用 W64DEVKIT_BIN 覆盖。", file=sys.stderr)
        sys.exit(rc)
    if out:
        print(f"  -> {REL(out)}（{os.path.getsize(out)} B）")


def need(path: str, hint: str) -> None:
    if not os.path.exists(path):
        print(f"FAIL 缺少 {REL(path)}。\n  {hint}", file=sys.stderr)
        sys.exit(1)


def sha256(path: str) -> str:
    try:
        with open(path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()
    except OSError as e:
        # 读的是**刚被本脚本写出来**的产物：读不到就是这一步没产出（不是环境缺件，所以是 1 不是 2）
        print(f"FAIL 读不了刚产出的 {REL(path)}。\n  {e}", file=sys.stderr)
        sys.exit(1)


# ---------------------------------------------------------------- 各目标

def step_objs() -> None:
    print("== objs ==")
    # 不加 -fno-exceptions：既有产物就是 GCC 默认（异常开着，assemble.o 里能看到
    # __gxx_personality_seh0）编的，加/不加会得到不同的目标文件。
    for src, obj in [
        # 装配器核心：将来壳与 CLI 共用（纯 C++17，不依赖 LLVM）
        ("simplexc/assemble/assemble.cc", "assemble.o"),
        # 游戏 jmp 进来的最外层入口桩 + ___chkstk_ms（.S 走预处理汇编，不需要 C++ 标准）
        ("simplexc/shell/entry_shim.S", "entry_shim.o"),
        # 路线 B 的 runtime 最小一块：thread_exit（会被链进 blob）
        ("simplexc/runtime/rt_min.cc", "rt_min.o"),
        # spike 的程序根（进 blob 的可观测痕迹）
        ("simplexc/cli/prog_empty.cc", "prog_empty.o"),
    ]:
        flags = [] if src.endswith(".S") else ["-std=c++17", "-O2"]
        out = os.path.join(OBJDIR, obj)
        run([CXX, *flags, "-c", src, "-o", REL(out)], out=out,
            hint=f"编译 {src} 失败 → 先看它的 include 是否还在；这条命令的 flags 是从既有 "
                 f"build/objs/{obj} 反推出来的（改了就没有逐字节可复现性）。")


def step_tcblob() -> None:
    print("== tcblob ==")
    out = os.path.join(BUILD, "tcblob.exe")
    # -static：w64devkit 默认就是静态链 libstdc++/libgcc（实测加不加产物逐字节相同），
    # 显式写上是为了换工具链时不悄悄多出 DLL 依赖。
    run([CXX, "-std=c++17", "-O2", "-static",
         "simplexc/cli/tcblob.cc", "build/objs/assemble.o", "-o", REL(out)], out=out,
        hint="链接失败 → 确认 build/objs/assemble.o 已由 `objs` 目标生成。")


def step_llvm_prog() -> None:
    print("== llvm_prog ==")
    need(os.path.join(LLVM_INC, "llvm-c", "Core.h"),
         "先取 LLVM：python tools/fetch_llvm.py --dest D:/downloads（build/llvm 不进 git）")
    need(os.path.join(LLVM_LIB, "libLLVM-C.a"),
         "缺 MinGW 导入库 libLLVM-C.a：见 docs/facts.md §构建与 LLVM（gendef + dlltool）")
    out = os.path.join(BUILD, "llvm_prog.exe")
    # -L/-I 必须相对（见模块 docstring）；运行期要 PATH 里有 build/llvm/bin 才找得到 LLVM-C.dll
    run([CXX, "-std=c++17", "-O2", "-static",
         "-I", REL(LLVM_INC), "simplexc/cli/llvm_prog.cc",
         "-o", REL(out), "-L", REL(LLVM_LIB), "-lLLVM-C"], out=out,
        hint="链接失败 → 看 build/llvm/lib/libLLVM-C.a 是不是那份自造导入库。")


def step_hook() -> None:
    print("== hook / simplexc_dll ==")
    # 转发层 = **C 壳**（tools/compile_hook.c）+ **C++ 编译器核心**（frontend/sema/codegen/assemble/shell）。
    # 壳仍是单文件 C，但它现在链接核心，所以构建从「一条 gcc 命令」变成三步，
    # 而且链接要用 g++（要 C++ 运行库）。两个名字是同一个目标：
    #   `hook` 是既有名字（`all` 里有它）；`simplexc_dll` 是与其它 simplexc 目标对齐的名字。
    # 部署入口 tools/deploy_compile_hook.py 转调本 target（只留一份构建命令）。
    out = os.path.join(BUILD, "compile_hook.dll")            # 绝对路径，见 docstring 第 2 条

    # 1) 构建期把两个装配用的目标文件嵌进 DLL（游戏进程里没有 build/objs/）
    gen = os.path.join(SIMPLEXC, "tools", "gen_embedded_objs.py")
    run([sys.executable, gen], hint="生成 embedded_objs.cc 失败 —— 先看 build/objs 里有没有"
        " entry_shim.o / rt_min.o（本目标的依赖里已经带了 objs）。")

    # 2) C 壳：**用 gcc**（不能交给 g++：g++ 会把 .c 当 C++ 编）
    shell_obj = os.path.join(OBJDIR, "compile_hook_shell.o")
    run([CC, "-c", "-O2", "tools/compile_hook.c", "-o", REL(shell_obj)], out=shell_obj,
        hint="编译 C 壳失败 → 它的 include 只需要 ../simplexc/shell/simplexc_core.h。")

    # 3) 链接：g++（C++ 运行库）+ LLVM-C 导入库；-static 把 libstdc++/libgcc/libwinpthread
    #    一起包进来，否则游戏目录还要多拷三个 DLL。
    run([CXX, "-shared", "-O2", "-static", "-o", out, REL(shell_obj),
         "simplexc/shell/simplexc_core.cc",
         "simplexc/shell/embedded_objs.cc",
         *CORE_SOURCES,
         "-I", "simplexc", "-I", REL(LLVM_INC),
         "-L", REL(LLVM_LIB), "-lLLVM-C"], out=out,
        hint="链接失败 → 确认 build/llvm/lib/libLLVM-C.a 与 build/objs/assemble.o 都在"
             "（后者由 objs 目标生成）。-static 与 -lLLVM-C 可以共存；真冲突就只剩去掉 -static。")


def step_core_selftest() -> None:
    print("== core_selftest ==")
    out = os.path.join(BUILD, "core_selftest.exe")
    # 判据入口（不进 DLL）：直接把源码字节喂给 `simplexc_compile` 并落盘，供宿主跑。
    # 与 DLL 同一套核心、同一套源文件表 —— 否则判据判的不是交付物。
    run([CXX, "-std=c++17", "-O2", "simplexc/shell/core_selftest.cc",
         "simplexc/shell/simplexc_core.cc",
         "simplexc/shell/embedded_objs.cc",
         *CORE_SOURCES,
         "-I", "simplexc", "-I", REL(LLVM_INC),
         "-L", REL(LLVM_LIB), "-lLLVM-C",
         "-o", REL(out)], out=out,
        hint="链接失败 → 先跑 `build.py objs`（embedded_objs.cc 要它）与取 LLVM（build/llvm）。")


def step_hir_selftest() -> None:
    print("== hir_selftest ==")
    out = os.path.join(BUILD, "hir_selftest.exe")
    # sema 的自检入口（不进产品路径）：只要 sema 自己的三个文件，不链前端、不链 LLVM ——
    # 它的 golden 是 `simplexc/sema/testdata/hir_sample.dump.txt`，路径由 `__FILE__` 推出，
    # 所以 `-I simplexc/sema` 与相对源文件路径都不能省（见 sema/hir_selftest.cc 的 golden_path）。
    run([CXX, "-std=c++17", "-O2", "-static",
         "-I", "simplexc/sema",
         "simplexc/sema/hir_selftest.cc",
         "simplexc/sema/hir_dump.cc",
         "simplexc/sema/types.cc",
         "-o", REL(out)], out=out,
        hint="编译失败 → 看 simplexc/sema/*.{cc,hh} 的报错；这个目标只用 C++17 标准库"
             "（不需要 LLVM，也不需要前端）。")


def step_analyze_selftest() -> None:
    print("== analyze_selftest ==")
    out = os.path.join(BUILD, "analyze_selftest.exe")
    # sema 的第二个判据入口（不进产品路径）：`.si` → 前端 AST → `sema::analyze` → dump，
    # 与 `sema/testdata/cases/*.dump` 逐字比 + 六条不变量。**不链 LLVM**：sema 里本来就没有
    # LLVM（`grep -c LLVM simplexc/sema/` 为 0），判据要把这件事当场证明。
    # 链前端是因为输入是源码；`.si` → AST 那一段粘合属于判据侧（见 analyze_selftest.cc 头注释）。
    run([CXX, "-std=c++17", "-O2", "-static",
         "-I", "simplexc/sema",
         "simplexc/sema/analyze_selftest.cc",
         "simplexc/sema/analyze.cc",
         "simplexc/sema/layout.cc",
         "simplexc/sema/hir_dump.cc",
         "simplexc/sema/types.cc",
         "simplexc/frontend/decls.cc",
         "simplexc/frontend/lexer.cc",
         "simplexc/frontend/parser.cc",
         "simplexc/frontend/frontend.cc",
         "-o", REL(out)], out=out,
        hint="编译失败 → 看 simplexc/sema/ 或 simplexc/frontend/ 的报错；这个目标只用 C++17"
             " 标准库（不需要 LLVM）。")


def step_ast_dump() -> None:
    print("== ast_dump ==")
    out = os.path.join(BUILD, "ast_dump.exe")
    # 前端（simplexc/frontend/*.cc）的 C++17 实现 + 它自己的调试 main：不依赖 LLVM，
    # 也不链进产品 DLL（判据入口，不是产线）。
    run([CXX, "-std=c++17", "-O2", "-static",
         "simplexc/frontend/ast_dump_main.cc",
         "simplexc/frontend/decls.cc",
         "simplexc/frontend/lexer.cc",
         "simplexc/frontend/parser.cc",
         "simplexc/frontend/frontend.cc",
         "simplexc/frontend/ast_dump.cc",
         "simplexc/frontend/json.cc",
         "-o", REL(out)], out=out,
        hint="编译前端失败 → 看 frontend/*.cc 的报错；它只用 C++17 标准库（不需要 LLVM）。")


def step_simplexc_cli() -> None:
    print("== simplexc_cli ==")
    out = os.path.join(BUILD, "simplexc_cli.exe")
    # M4.5 的产品路径入口：frontend + lower + assemble，`.si` -> blob。
    # 源文件表与 DLL 共用 CORE_SOURCES（壳要的是同一套核心）。
    run([CXX, "-std=c++17", "-O2",
         "-I", "simplexc", "-I", LLVM_INC,
         "simplexc/cli/simplexc_cli.cc",
         *CORE_SOURCES,
         "-L", LLVM_LIB, "-lLLVM-C",
         "-o", REL(out)], out=out,
        hint="编译 CLI 失败 → 需要 build/llvm/{include,lib}（头 + 导入库）；静态库顺序要压在源文件之后。")

    # Windows 先从 exe 所在目录找依赖 DLL（再看 PATH）→ 把 LLVM-C.dll 放它旁边，
    # 判据入口（check.py）就不用为这一步改 PATH。
    dll = os.path.join(LLVM_BIN, "LLVM-C.dll")
    if os.path.exists(dll):
        shutil.copy2(dll, os.path.join(BUILD, "LLVM-C.dll"))
    else:
        print(f"  警告：找不到 {dll}，CLI 跑起来会缺依赖")


def step_codegen_selftest() -> None:
    print("== codegen_selftest ==")
    need(os.path.join(LLVM_INC, "llvm-c", "Core.h"),
         "先取 LLVM：python tools/fetch_llvm.py --dest D:/downloads（build/llvm 不进 git）")
    out = os.path.join(BUILD, "codegen_selftest.exe")
    # codegen 的判据入口（不进产品路径）：`.si` 走产品链（Frontend → build_module → analyze →
    # `codegen::Program`）出 IR，再逐条查 `codegen/testdata/cases/*.irkeys`。要链 LLVM 是因为
    # 被测对象（codegen.cc + llvm_ffi.cc）本来就在 LLVM 上；不用 assemble.o —— 它只出 IR，
    # 不出产物（产物那一路由 blob_baseline 判）。
    run([CXX, "-std=c++17", "-O2", "-static",
         "-I", "simplexc", "-I", REL(LLVM_INC),
         "simplexc/codegen/codegen_selftest.cc",
         "simplexc/frontend/decls.cc",
         "simplexc/frontend/lexer.cc",
         "simplexc/frontend/parser.cc",
         "simplexc/frontend/frontend.cc",
         "simplexc/codegen/module_build.cc",
         "simplexc/codegen/codegen.cc",
         "simplexc/codegen/asm_x64.cc",
         "simplexc/codegen/llvm_ffi.cc",
         "simplexc/sema/analyze.cc",
         "simplexc/sema/layout.cc",
         "simplexc/sema/types.cc",
         "-L", REL(LLVM_LIB), "-lLLVM-C",
         "-o", REL(out)], out=out,
        hint="链接失败 → 确认 build/llvm/{include,lib} 都在（LLVM 固定 23.1.3，见 docs/facts.md）；"
             "运行期要 PATH 里有 build/llvm/bin 或 build/LLVM-C.dll 才找得到 LLVM-C.dll。")


def difftrace_sources() -> list[str]:
    """`difftrace` 目标的源文件表：目录里除 `*_selftest.cc` 之外的 .cc。

    按目录列而不是写死清单：这里的唯一规则是「自测文件自带 main，不能进 exe」，
    写死清单则每加一个 .cc 都要回来改一处，漏了就是重复定义的链接错误。
    """
    need(DIFFTRACE_DIR, "build.py 按目录列 difftrace 的源文件，这个目录不见了就没法编。")
    return [REL(os.path.join(DIFFTRACE_DIR, n))
            for n in sorted(os.listdir(DIFFTRACE_DIR))
            if n.endswith(".cc") and not n.endswith("_selftest.cc")]


def need_difftrace_deps() -> None:
    for path in (os.path.join(DIFFTRACE_DEPS, "unicorn", "include", "unicorn", "unicorn.h"),
                 os.path.join(DIFFTRACE_DEPS, "unicorn", "lib", "libunicorn.a"),
                 os.path.join(DIFFTRACE_DEPS, "capstone", "include", "capstone", "capstone.h"),
                 os.path.join(DIFFTRACE_DEPS, "capstone", "lib", "libcapstone.a")):
        need(path, DIFFTRACE_DEPS_HINT)


def step_difftrace() -> None:
    print("== difftrace ==")
    # 缺 SDK 只该让这一个目标失败（所以它不在 `all` 里，见模块 docstring）。
    need_difftrace_deps()
    out = os.path.join(BUILD, "difftrace.exe")
    run([CXX, "-O2", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic",
         "-I", REL(os.path.join(DIFFTRACE_DEPS, "unicorn", "include")),
         "-I", REL(os.path.join(DIFFTRACE_DEPS, "capstone", "include")),
         *difftrace_sources(),
         "-L", REL(os.path.join(DIFFTRACE_DEPS, "unicorn", "lib")), "-lunicorn",
         "-L", REL(os.path.join(DIFFTRACE_DEPS, "capstone", "lib")), "-lcapstone",
         "-o", REL(out)], out=out,
        hint="编译/链接失败 → 看 simplexc/tools/difftrace/*.cc 的报错；SDK 头与导入库由 "
             "fetch_deps.py 生成（--force 可重建）。")
    # Windows 先从 exe 所在目录找依赖 DLL → 两个 dll 放 build/ 旁边，跑判据不用改 PATH
    for name in ("unicorn", "capstone"):
        dll = os.path.join(DIFFTRACE_DEPS, name, "lib", name + ".dll")
        need(dll, DIFFTRACE_DEPS_HINT)
        shutil.copy2(dll, os.path.join(BUILD, name + ".dll"))
        print(f"  -> {REL(os.path.join(BUILD, name + '.dll'))}")


def step_difftrace_selftest() -> None:
    print("== difftrace_selftest ==")
    out = os.path.join(BUILD, "difftrace_selftest.exe")
    # 判据核心（diff.*）不碰 Unicorn/Capstone —— 这个目标因此不需要 SDK，能单独跑。
    run([CXX, "-O2", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic",
         REL(os.path.join(DIFFTRACE_DIR, "diff.cc")),
         REL(os.path.join(DIFFTRACE_DIR, "diff_selftest.cc")),
         "-o", REL(out)], out=out,
        hint="编译失败 → 看 simplexc/tools/difftrace/{diff,diff_selftest}.cc 的报错。")
    # 用例 FAIL 是判据的结论（不是构建出错）：退出码原样透传，别折叠成 1。
    print("$ " + REL(out))
    rc = run_rc([out])
    if rc != 0:
        print(f"FAIL 自测有用例不通过：{REL(out)} 返回 {rc}", file=sys.stderr)
        sys.exit(rc)


def step_asm_x64_selftest() -> None:
    print("== asm_x64_selftest ==")
    out = os.path.join(BUILD, "asm_x64_selftest.exe")
    # stdlib `asm` 块编码器（`asm_x64.*`）的判据入口（不进产品路径）：只编这两个 .cc，
    # 不链前端、不链 LLVM、不链 assemble.o —— 被测对象是纯函数，编不过就是它自己的问题。
    # **不在 `all` 里**（同 difftrace_selftest）：它还没接进编译流水线，`all` 不该为它变慢。
    run([CXX, "-std=c++17", "-O2", "-static", "-Wall", "-Wextra", "-Wpedantic",
         "simplexc/codegen/asm_x64.cc",
         "simplexc/codegen/asm_x64_selftest.cc",
         "-o", REL(out)], out=out,
        hint="编译失败 → 看 simplexc/codegen/asm_x64*.cc 的报错；这个目标只用 C++17 标准库。")
    # 用例 FAIL 是判据的结论（不是构建出错）：退出码原样透传，别折叠成 1。
    print("$ " + REL(out))
    rc = run_rc([out])
    if rc != 0:
        print(f"FAIL 自测有用例不通过：{REL(out)} 返回 {rc}", file=sys.stderr)
        sys.exit(rc)


def step_spike() -> None:
    print("== spike ==")
    out = os.path.join(BUILD, "blob_empty.bin")
    # --obj 顺序 = 链接顺序；entry 是 shim 的符号。这条命令与 M1 spike 记录的一致，
    # 产出的 blob 与 M1 那份 sha256 相同（见 docs/notes/2026-10-17-m1-spike-游戏内.md）。
    run(["build/tcblob.exe",
         "--obj", "build/objs/rt_min.o",
         "--obj", "build/objs/entry_shim.o",
         "--obj", "build/objs/prog_empty.o",
         "--entry", "__tc_entry", "--out", "build/blob_empty.bin"], out=out,
        hint="装配失败 → tcblob 的报错里带 status（BadObject=COFF 解析、UndefinedSymbol="
             "ExternalResolver 解不了、UnsupportedReloc=非 REL32 重定位）。")
    print(f"  sha256 {sha256(out)}")
    print("  注意：空程序只证明链路通，**不要**部署进游戏（docs/pitfalls.md）。")


STEPS = {
    "simplexc_cli": step_simplexc_cli,
    "objs": step_objs,
    "tcblob": step_tcblob,
    "llvm_prog": step_llvm_prog,
    "hook": step_hook,
    "simplexc_dll": step_hook,        # 同一个目标的两个名字（见 step_hook 的注释）
    "core_selftest": step_core_selftest,
    "hir_selftest": step_hir_selftest,
    "analyze_selftest": step_analyze_selftest,
    "ast_dump": step_ast_dump,
    "codegen_selftest": step_codegen_selftest,
    "difftrace": step_difftrace,
    "difftrace_selftest": step_difftrace_selftest,
    "asm_x64_selftest": step_asm_x64_selftest,
    "spike": step_spike,
}
ALL = ["objs", "tcblob", "llvm_prog", "hook", "ast_dump", "hir_selftest", "analyze_selftest",
       "codegen_selftest"]
DEPENDS = {"tcblob": ["objs"], "spike": ["objs", "tcblob"],
           "hook": ["objs"], "simplexc_dll": ["objs"], "core_selftest": ["objs"]}

# 并发时真正要排的先后。DEPENDS 只管「不指定也会自己补做」，排不出先后；串行时剩下的错序靠
# `wanted` 的构造顺序恰好避开了，并发时不管就是两个 g++ 同时碰同一个文件：
#   simplexc_cli ← objs        CLI 链 build/objs/assemble.o，而 objs 会重写它
#   core_selftest ← hook       core_selftest 编的 simplexc/shell/embedded_objs.cc 由 hook 生成
#   simplexc_dll ← hook        两个名字是**同一个**step_hook：两路同时跑会各自重写
#                              embedded_objs.cc 又各自编它
SCHEDULE_DEPS = {**{name: list(deps) for name, deps in DEPENDS.items()},
                 "simplexc_cli": ["objs"], "core_selftest": ["objs", "hook"],
                 "simplexc_dll": ["objs", "hook"]}


def _run_step(name: str) -> Sink:
    """在一个工作线程里跑一个目标，把它的输出与退出码收进 Sink。"""
    sink = Sink()
    _TLS.sink = sink
    started = time.monotonic()
    try:
        STEPS[name]()
    except SystemExit as e:
        # run/need/sha256 的失败路径：FAIL 行已经进缓冲了，这里只接住退出码
        sink.rc = e.code if isinstance(e.code, int) and e.code else 1
    except Exception:                       # 步骤本身炸了也要留下现场，不能静默算成功
        sink.add("err", traceback.format_exc().encode("utf-8", "replace"))
        sink.rc = 1
    finally:
        sink.seconds = time.monotonic() - started
        _TLS.sink = None
    return sink


def run_parallel(names: list[str], jobs: int) -> int:
    """按依赖关系并发跑这些目标，返回退出码（第一个失败目标的，没失败就是 0）。"""
    deps = {name: [d for d in SCHEDULE_DEPS.get(name, []) if d in names] for name in names}
    results: dict[str, Sink] = {}
    futures: dict[Future[Sink], str] = {}
    started: set[str] = set()
    real_out, real_err = sys.stdout, sys.stderr
    sys.stdout, sys.stderr = Capture(real_out, "out"), Capture(real_err, "err")
    try:
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            while len(results) < len(names):
                for name in names:
                    if name in started or any(d not in results for d in deps[name]):
                        continue
                    started.add(name)
                    futures[pool.submit(_run_step, name)] = name
                if not futures:         # 表里成环才会到这（否则总有东西在跑或可起）
                    raise RuntimeError("依赖成环：" + ", ".join(n for n in names if n not in results))
                finished, _ = wait(futures, return_when=FIRST_COMPLETED)
                for future in finished:
                    name = futures.pop(future)
                    results[name] = future.result()
                    # 进度进 stderr：长目标（hook）跑的时候得看得出还没卡死
                    print(f"  [{len(results)}/{len(names)}] {name} "
                          f"{'ok' if results[name].rc == 0 else 'FAIL'} "
                          f"{results[name].seconds:.1f}s", file=real_err, flush=True)
    finally:
        sys.stdout, sys.stderr = real_out, real_err

    # 整块倒出：按命令行顺序，失败目标的 FAIL 行就在它那一块里，能看出是哪个目标
    for name in names:
        _flush_sink(results[name])
    failed = [(name, results[name].rc) for name in names if results[name].rc != 0]
    if failed:
        print("FAIL 并发构建有目标失败：" +
              "、".join(f"{name}（退出码 {rc}）" for name, rc in failed), file=sys.stderr)
        print("  每个目标的完整输出已按命令行顺序印在上面；重跑单个目标可以用 "
              "`--jobs 1 <目标>` 排除并发因素。", file=sys.stderr)
        return failed[0][1]
    print("全部完成：" + ", ".join(names))
    return 0


def main(argv: list[str]) -> int:
    global CXX, CC
    ap = argparse.ArgumentParser(
        description="simplexc 的一键构建（产物都在 reversing/build/，不进 git）",
        epilog="all = " + "+".join(ALL) + "；spike = 空程序 blob（单独，依赖 " +
               "+".join(DEPENDS["spike"]) + "）；不在 all 里的：" +
               " | ".join(t for t in STEPS if t not in ALL) +
               "（difftrace 要 difftrace C SDK，all 的成功不依赖它）")
    ap.add_argument("targets", nargs="*", default=[],
                    help="all | " + " | ".join(STEPS) + "（默认 all）")
    ap.add_argument("--jobs", type=int, default=None, metavar="N",
                    help=f"同时跑几个目标（默认 = CPU 核数，上限 {MAX_JOBS}；"
                         f"--jobs 1 = 串行，与加并行之前的行为一致）")
    args = ap.parse_args(argv)

    if args.jobs is None:
        jobs = min(os.cpu_count() or 1, MAX_JOBS)
    elif args.jobs < 1 or args.jobs > MAX_JOBS:
        print(f"FAIL --jobs 只接受 1..{MAX_JOBS}：每路 g++ 峰值几百 MB，再多只是抢内存"
              f"（同一个上限的来历见 simplexc/tools/m5/batch.py 的 MAX_JOBS）。", file=sys.stderr)
        return 2
    else:
        jobs = args.jobs

    wanted = []
    for t in (args.targets or ["all"]):
        if t != "all" and t not in STEPS:
            print(f"FAIL 不认识的目标 {t}\n  可用：all | " + " | ".join(STEPS), file=sys.stderr)
            return 2
        for name in (ALL if t == "all" else [t]):
            # 依赖先做（tcblob 要 assemble.o，spike 要 tcblob），同一目标只做一次
            for dep in DEPENDS.get(name, []):
                if dep not in wanted:
                    wanted.append(dep)
            if name not in wanted:
                wanted.append(name)

    CXX = tool("g++.exe", "W64DEVKIT_CXX")
    CC = tool("gcc.exe", "W64DEVKIT_CC")
    for path, env in ((CXX, "W64DEVKIT_CXX"), (CC, "W64DEVKIT_CC")):
        if not os.path.exists(path):
            print(f"FAIL 找不到 {path}\n  用环境变量 {env} 指向可执行文件，"
                  f"或 W64DEVKIT_BIN 指向工具链目录（默认 D:/bin/w64devkit/bin）。",
                  file=sys.stderr)
            return 1
    try:
        os.makedirs(OBJDIR, exist_ok=True)
    except OSError as e:
        print(f"FAIL 建不了中间目录 {REL(OBJDIR)}。\n  {e}\n"
              f"  它的位置在本脚本里写死；那条路径上被占成普通文件时只能先挪开它。",
              file=sys.stderr)
        sys.exit(2)

    print(f"工具链 {CXX}")
    if jobs == 1:
        # 串行：加并行之前的原样路径（子进程直连终端、失败立即退），当参照物用
        for name in wanted:
            STEPS[name]()
        print("全部完成：" + ", ".join(wanted))
        return 0

    print(f"并发 {jobs} 路：" + ", ".join(wanted))
    return run_parallel(wanted, jobs)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

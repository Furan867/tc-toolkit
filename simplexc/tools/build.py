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

目标
----
    objs       build/objs/{assemble,entry_shim,rt_min,prog_empty}.o
    tcblob     build/tcblob.exe        装配器 CLI（--run 可不进游戏自测一次产物）
    llvm_prog  build/llvm_prog.exe     LLVM-C 生成程序根（M1 那条链的样本）
    hook       build/compile_hook.dll  转发层（tools/compile_hook.c）
    ast_dump   build/ast_dump.exe     前端的判据入口（frontend/ 的 C++17 实现，不进产品 DLL）
    spike      build/blob_empty.bin (+ .entry)  「空程序」blob —— 只证明链路通，
               不能拿进游戏（会让游戏在下一次编译请求时 abort，见 docs/pitfalls.md）

环境变量
--------
    W64DEVKIT_BIN   工具链目录，默认 D:/bin/w64devkit/bin
    W64DEVKIT_CXX / W64DEVKIT_CC   直接指定 g++ / gcc（优先于 W64DEVKIT_BIN 里的）

前置：LLVM（build/llvm/，不进 git）
----
    llvm_prog 需要 build/llvm/include（头）与 build/llvm/lib/libLLVM-C.a（MinGW 导入库）。
    没有就先取：python tools/fetch_llvm.py --dest D:/downloads（见 docs/facts.md §构建与 LLVM）。

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
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SIMPLEXC = os.path.dirname(HERE)
REPO = os.path.dirname(SIMPLEXC)                       # reversing/

BUILD = os.path.join(REPO, "build")
OBJDIR = os.path.join(BUILD, "objs")
# LLVM-C.dll + 头 + 自造 MinGW 导入库（不进 git，见 docs/facts.md §构建与 LLVM）
LLVM_INC = os.path.join(BUILD, "llvm", "include")
LLVM_LIB = os.path.join(BUILD, "llvm", "lib")

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


def run(cmd: list[str], out: str | None = None, hint: str = "") -> None:
    """跑一条命令；非 0 立即退出（附上这条命令的可执行解释）。"""
    print("$ " + " ".join(cmd))
    rc = subprocess.run(cmd, cwd=REPO).returncode
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
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


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
    print("== hook ==")
    out = os.path.join(BUILD, "compile_hook.dll")            # 绝对路径，见 docstring 第 2 条
    # 与 tools/deploy_compile_hook.py 的 build 走的同一条 gcc 命令（那份是部署用的入口，
    # 这里只是让 build.py 也能单独重建 DLL；部署仍然只走 deploy_compile_hook.py）。
    run([CC, "-shared", "-O2", "-static", "-o", out, "tools/compile_hook.c"], out=out,
        hint="编译转发层失败 → 它是纯 C（不用 g++）；路径与 deploy_compile_hook.py 保持一致。")


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
    # 只有这一条命令会把三段串起来（判据入口与将来的壳都复用它）。
    run([CXX, "-std=c++17", "-O2",
         "-I", "simplexc", "-I", LLVM_INC,
         "simplexc/cli/simplexc_cli.cc",
         "simplexc/frontend/decls.cc",
         "simplexc/frontend/lexer.cc",
         "simplexc/frontend/parser.cc",
         "simplexc/frontend/frontend.cc",
         "simplexc/frontend/ast_dump.cc",
         "simplexc/frontend/json.cc",
         "simplexc/lower/lower.cc",
         "simplexc/lower/llvm_ffi.cc",
         "simplexc/assemble/assemble.cc",
         "-L", LLVM_LIB, "-lLLVM-C",
         "-o", REL(out)], out=out,
        hint="编译 CLI 失败 → 需要 build/llvm/{include,lib}（头 + 导入库）；静态库顺序要压在源文件之后。")


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
    "ast_dump": step_ast_dump,
    "spike": step_spike,
}
ALL = ["objs", "tcblob", "llvm_prog", "hook", "ast_dump"]
DEPENDS = {"tcblob": ["objs"], "spike": ["objs", "tcblob"]}      # spike 单独跑时自动补做


def main(argv: list[str]) -> int:
    global CXX, CC
    ap = argparse.ArgumentParser(
        description="simplexc 的一键构建（产物都在 reversing/build/，不进 git）",
        epilog="all = " + "+".join(ALL) + "；spike = 空程序 blob（单独，依赖 " +
               "+".join(DEPENDS["spike"]) + "）")
    ap.add_argument("targets", nargs="*", default=[],
                    help="all | " + " | ".join(STEPS) + "（默认 all）")
    args = ap.parse_args(argv)

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
    os.makedirs(OBJDIR, exist_ok=True)

    print(f"工具链 {CXX}")
    for name in wanted:
        STEPS[name]()
    print("全部完成：" + ", ".join(wanted))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

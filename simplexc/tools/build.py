#!python3
"""simplexc 的构建入口：**转调 CMake**。构建定义（目标、源文件表、flags、依赖）只有
`CMakeLists.txt` 一处 —— 这条路上再放第二份编译命令行时，「判据编出来的」与「交付物」
就可能不是同一份。

本文件只做两件事：`build/cmake/CMakeCache.txt` 不存在时 configure（`cmake --preset mingw`），
再 `cmake --build build/cmake --target <每个目标>`。调用点（check.py / m5/batch.py /
blob_baseline.py / 各 README）与产物路径 `build/<名字>` 是外部契约，所以这层壳留着。
目标之间的先后（`spike` 要 objs + tcblob、`hook` 要 objs）由 CMake 的依赖表达，不用手排。

用法：`build.py`（= all）· `build.py objs tcblob`（只做这几步）· `build.py --list` ·
`build.py --jobs 1`（串行，排查用）

目标（产物路径见 CMakeLists.txt 各小节）
    all 里：objs（build/objs/*.o，会被嵌进 DLL）· tcblob · llvm_prog · hook
            （build/compile_hook.dll）· ast_dump · hir_selftest · analyze_selftest · codegen_selftest
    不在 all 里：simplexc_dll（同 hook）· core_selftest · spike（build/blob_empty.bin：
            **不要**部署进游戏，见 docs/pitfalls.md）· simplexc_cli · difftrace ·
            difftrace_selftest · asm_x64_selftest

环境变量与前置（两份资产都不进 git）：`W64DEVKIT_BIN`（默认 D:/bin/w64devkit/bin）·
LLVM 要 `build/llvm/{include,lib}`（fetch_llvm.py；缺它 configure 直接失败）· difftrace 的
C SDK 要 `work/tools/difftrace-deps/`（fetch_deps.py；缺它只有该目标不可用，hint 原文照印）。

基线：flags 是照旧构建逐条搬的（从既有产物**反推确认**过，见 docs/notes/2026-10-17-build-script.md），
build/objs/*.o 与它逐字节相同（会被原样嵌进 DLL）。exe/dll 会差几十字节 —— CMake 总会多传
`--out-implib` 与默认 Windows 库，这是链接行的固有差异，不是 flags 变了；判据按 check.py，不按字节比。
产物的 `-o` 用绝对路径：ld 给 DLL 挑的默认 image base 随输出路径字符串变，换一种拼法就差 ~150 字节。
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))          # reversing/
CMAKE_BUILD = os.path.join(REPO, "build", "cmake")     # = CMakePresets.json 的 binaryDir

# 与 m5/batch.py 的 MAX_JOBS 同值（difftrace 实测 16 路只比 12 路快 7%、多占 1.75 GB）：
# 编译器是同一类「每进程几百 MB、再多只抢内存」的活。
MAX_JOBS = 12

# 与 CMakeLists.txt 头部的清单一一对应；其余目标在 CMake 里是 EXCLUDE_FROM_ALL（要显式 --target）。
ALL = ("objs", "tcblob", "llvm_prog", "hook", "ast_dump", "hir_selftest",
       "analyze_selftest", "codegen_selftest")
TARGETS = ALL + ("simplexc_dll", "core_selftest", "spike", "simplexc_cli", "difftrace",
                 "difftrace_selftest", "asm_x64_selftest")


def cmake() -> str:
    exe = shutil.which("cmake")
    if exe is None:
        raise SystemExit("FAIL 找不到 cmake —— 构建定义在 CMakeLists.txt，先装 CMake（3.20+）+ Ninja。")
    return exe


def configure_command(cache_args=()) -> list[str]:
    # `cache_args` 是额外 -D：判据 hook_offline.py 用它把自己的工作目录传进测试壳的路径宏。
    return [cmake(), "--preset", "mingw", *cache_args]


def build_command(targets=(), jobs=None) -> list[str]:
    """`targets` 为空 = CMake 的默认构建（= 上面 ALL 那 8 个）。"""
    cmd = [cmake(), "--build", CMAKE_BUILD]
    if targets:
        cmd += ["--target", *targets]
    if jobs:
        cmd += ["--parallel", str(jobs)]
    return cmd


def run(cmd: list[str]) -> int:
    print("$ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=REPO).returncode


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="simplexc 的一键构建（转调 CMake，产物在 reversing/build/）："
                                 "all = " + "+".join(ALL))
    ap.add_argument("targets", nargs="*", default=[],
                    help="all | " + " | ".join(TARGETS) + "（默认 all）")
    ap.add_argument("--jobs", type=int, default=None, metavar="N",
                    help=f"同时跑几个编译进程（默认 = 机器核数；上限 {MAX_JOBS}）")
    ap.add_argument("--list", action="store_true", help="列出全部目标名")
    args = ap.parse_args(argv)

    if args.list:
        print("\n".join(f"{n:<18}{'' if n in ALL else '（不在 all 里）'}" for n in TARGETS))
        return 0

    if args.jobs is not None and not 1 <= args.jobs <= MAX_JOBS:
        print(f"FAIL --jobs 只接受 1..{MAX_JOBS}：每路编译器峰值几百 MB，再多只是抢内存。", file=sys.stderr)
        return 2

    names = list(args.targets) or ["all"]
    unknown = [t for t in names if t != "all" and t not in TARGETS]
    if unknown:
        print(f"FAIL 不认识的目标 {' '.join(unknown)}\n  可用：all | " + " | ".join(TARGETS), file=sys.stderr)
        return 2

    cache = os.path.join(CMAKE_BUILD, "CMakeCache.txt")
    if not os.path.exists(cache) and (rc := run(configure_command())):
        print("FAIL configure 失败 —— 上面是 CMake 的原话（缺工具链 / 缺 LLVM-C 都在那里说清）。", file=sys.stderr)
        return rc

    # `all` = 不带 --target 的默认构建；其余按名字建。两者一起给就分两次调。
    others = tuple(t for t in names if t != "all")
    for targets in ([()] if "all" in names else []) + ([others] if others else []):
        if rc := run(build_command(targets, args.jobs)):
            print(f"FAIL 构建失败（返回 {rc}）：{' '.join(targets) or 'all'}", file=sys.stderr)
            return rc
    print("全部完成：" + ", ".join(names))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

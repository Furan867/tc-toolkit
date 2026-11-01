#!python3
"""simplexc 的构建入口：**转调 CMake**。构建定义（目标、源文件表、flags、依赖）只在
`CMakeLists.txt` 一处 —— 这条路上再放第二份编译命令行时，「判据编出来的」与「交付物」
就可能不是同一份。

用法
    build.py               默认构建（= all 那 8 个目标）
    build.py <目标>...     只建这几个
    build.py clean         清掉构建产物（保留 build/cmake 的缓存）
    build.py -l, --list    列出全部目标

前置（两份资产都不进 git）：`W64DEVKIT_BIN`（默认 `D:/bin/w64devkit/bin`）·
LLVM 要 `build/llvm/{include,lib}`（`simplexc/tools/fetch_llvm.py`，缺它 configure 直接失败）·
difftrace 的 C SDK 要 `work/tools/difftrace-deps/`（`simplexc/tools/difftrace/fetch_deps.py`；
缺它只有该目标不可用，hint 原文照印）。

产物在 `build/<名字>`；`build/objs/*.o` 会被嵌进 DLL。判据走 `check.py`，不按字节比。
并行由 Ninja 自己决定（环境变量 `CMAKE_BUILD_PARALLEL_LEVEL` 可覆盖）。
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))          # tc-toolkit/
CMAKE_BUILD = os.path.join(REPO, "build", "cmake")     # = CMakePresets.json 的 binaryDir

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


def build_command(targets=()) -> list[str]:
    """`targets` 为空 = CMake 的默认构建（= 上面 ALL 那 8 个）。"""
    cmd = [cmake(), "--build", CMAKE_BUILD]
    if targets:
        cmd += ["--target", *targets]
    return cmd


def run(cmd: list[str]) -> int:
    print("$ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=REPO).returncode


def clean() -> int:
    """删掉 CMake 知道的构建产物；`build/cmake` 的缓存留着（下次增量快）。"""
    if not os.path.exists(os.path.join(CMAKE_BUILD, "CMakeCache.txt")):
        print("没有构建树（build/cmake 未 configure），无需清理。")
        return 0
    return run([cmake(), "--build", CMAKE_BUILD, "--target", "clean"])


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="simplexc 的一键构建（转调 CMake，产物在 build/）："
                                 "all = " + "+".join(ALL))
    ap.add_argument("targets", nargs="*", default=[],
                    help="all | clean | " + " | ".join(TARGETS) + "（默认 all）")
    ap.add_argument("-l", "--list", action="store_true", help="列出全部目标名")
    ap.add_argument("-c", "--clean", action="store_true", help="清掉构建产物（= build.py clean）")
    args = ap.parse_args(argv)

    if args.list:
        print("\n".join(f"{n:<18}{'' if n in ALL else '（不在 all 里）'}" for n in TARGETS))
        return 0

    if args.clean or args.targets == ["clean"]:
        if len(args.targets) > 1 or (args.clean and args.targets):
            print("FAIL clean 不能和别的目标一起给。", file=sys.stderr)
            return 2
        return clean()

    names = list(args.targets) or ["all"]
    unknown = [t for t in names if t != "all" and t not in TARGETS]
    if unknown:
        print(f"FAIL 不认识的目标 {' '.join(unknown)}\n  可用：all | clean | " + " | ".join(TARGETS),
              file=sys.stderr)
        return 2

    cache = os.path.join(CMAKE_BUILD, "CMakeCache.txt")
    if not os.path.exists(cache) and (rc := run(configure_command())):
        print("FAIL configure 失败 —— 上面是 CMake 的原话（缺工具链 / 缺 LLVM-C 都在那里说清）。", file=sys.stderr)
        return rc

    # `all` = 不带 --target 的默认构建；其余按名字建。两者一起给就分两次调。
    others = tuple(t for t in names if t != "all")
    for targets in ([()] if "all" in names else []) + ([others] if others else []):
        if rc := run(build_command(targets)):
            print(f"FAIL 构建失败（返回 {rc}）：{' '.join(targets) or 'all'}", file=sys.stderr)
            return rc
    print("全部完成：" + ", ".join(names))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

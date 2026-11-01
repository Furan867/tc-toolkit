#!python3
"""把装配用的两个目标文件（`entry_shim.o` / `rt_min.o`）编成 C++ 源码，供 DLL 链接。

为什么要嵌进 DLL
----------------
装配要有**三个**目标文件（模块 COFF + `entry_shim.o` + `rt_min.o`，接口 §4），
而壳在**游戏进程里**编译：那里没有 `build/objs/`，也没有安全的临时目录。
所以后两个在**构建期**变成字节数组链进 DLL —— 壳只管内存里的字节。

用法
----
    python simplexc/tools/gen_embedded_objs.py                      # 默认路径
    python simplexc/tools/gen_embedded_objs.py --objs-dir <dir> --out <file>

产物 `simplexc/shell/embedded_objs.cc` **不入库**（由 build.py 的 `simplexc_dll`
目标每次构建时重新生成）—— 它是派生物，入库就要跟 `build/objs/*.o` 的每次重编同步。
声明在同目录手写的 `embedded_objs.hh` 里（生成物与声明分开，两者对不上时是链接错误，不会静默）。
"""
from __future__ import annotations

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SIMPLEXC = os.path.dirname(HERE)
REPO = os.path.dirname(SIMPLEXC)

# 目标文件 → 生成的符号名（顺序 = blob 的装配顺序，见 cli/simplexc_cli.cc 与 shell/simplexc_core.cc）
OBJS = [
    ("entry_shim.o", "kEntryShimObj", "kEntryShimObjSize"),
    ("rt_min.o", "kRtMinObj", "kRtMinObjSize"),
]
PER_LINE = 12

HEADER = """\
/* embedded_objs.cc —— 装配用的两个目标文件（**生成物，别手改**）。
 *
 * 由 `simplexc/tools/gen_embedded_objs.py` 从 `build/objs/{entry_shim,rt_min}.o` 生成；
 * 为什么必须嵌进 DLL 见那个脚本的 docstring。声明在同目录的 `embedded_objs.hh` 里。
 */
#include "embedded_objs.hh"

namespace simplexc::shell {
"""


def read_obj(path: str) -> bytes:
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        raise SystemExit(f"读不了 {path}：{e}") from e


def array_lines(name: str, data: bytes) -> list[str]:
    out = [f"extern const unsigned char {name}[] = {{"]
    for i in range(0, len(data), PER_LINE):
        chunk = data[i:i + PER_LINE]
        out.append("    " + " ".join(f"0x{b:02X}," for b in chunk))
    out.append("};")
    return out


def generate(objs_dir: str) -> str:
    parts = [HEADER]
    for fname, arr, size in OBJS:
        path = os.path.join(objs_dir, fname)
        if not os.path.isfile(path):
            raise SystemExit(f"找不到 {path} —— 先跑 python simplexc/tools/build.py objs")
        data = read_obj(path)
        if not data:
            raise SystemExit(f"{path} 是空文件 —— 它不是编出来的目标文件？")
        parts.append(f"/* {fname}（{len(data)} 字节，构建期嵌入） */\n")
        parts.extend(line + "\n" for line in array_lines(arr, data))
        parts.append(f"extern const std::size_t {size} = {len(data)};\n\n")
    parts.append("}  // namespace simplexc::shell\n")
    return "".join(parts)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="build/objs/{entry_shim,rt_min}.o → shell/embedded_objs.cc")
    ap.add_argument("--objs-dir", default=os.path.join(REPO, "build", "objs"),
                    help="目标文件目录（默认 build/objs）")
    ap.add_argument("--out", default=os.path.join(SIMPLEXC, "shell", "embedded_objs.cc"),
                    help="生成物路径（默认 simplexc/shell/embedded_objs.cc）")
    args = ap.parse_args(argv)

    text = generate(args.objs_dir)
    try:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    except OSError as e:
        print(f"写不了 {args.out}：{e}", file=sys.stderr)
        return 1
    print(f"generated {args.out} ({len(text)} B)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

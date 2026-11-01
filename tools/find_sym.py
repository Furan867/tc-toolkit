#!python3
"""按模块 / 函数名 / 地址查询符号表。

数据源（均在 work/ 下，由 nm 与 pdata_bounds.py 生成）:
    symbols_raw.txt        nm 输出（函数名 + 地址）
    function_bounds.tsv    .pdata 函数边界（用于给出函数体大小）

用法
----
    python tools/find_sym.py --module model/simulation          # 列出该模块的函数
    python tools/find_sym.py --module sim --sub                 # 模块名子串匹配
    python tools/find_sym.py --name evaluate                    # 函数名子串匹配
    python tools/find_sym.py --module model/simulation --big 500 # 只看 ≥500 字节的
    python tools/find_sym.py --addr 0x1403d3380                 # 反查地址属于哪个函数

默认按函数体大小降序输出（大函数通常承载核心逻辑），`--limit` 控制条数。
"""
from __future__ import annotations

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORK = os.path.join(ROOT, "work")

# 复用 nim_demangle.py 的 demangle 实现（不执行其写文件部分）
_DEMANGLER = os.path.join(HERE, "nim_demangle.py")


def load_demangler():
    src = open(_DEMANGLER, encoding="utf-8").read()
    ns = {"re": re}
    exec(src[src.index("def decode_proc_name"):
             src.index("# ---------- 3. parse nm output")], ns)
    return ns["demangle_symbol"]


def load_bounds(exe_hint=None):
    """地址(VA) -> (end, size)"""
    path = os.path.join(WORK, "function_bounds.tsv")
    out = {}
    if not os.path.isfile(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            try:
                start = int(parts[0], 16)
                out[start] = int(parts[2])
            except ValueError:
                continue
    return out


def load_symbols(demangle):
    """产出 (module, funcname, addr)。funcname 用 demangle 后的可读名。"""
    path = os.path.join(WORK, "symbols_raw.txt")
    if not os.path.isfile(path):
        print(f"错误: 找不到 {path}，请先重取符号表", file=sys.stderr)
        sys.exit(1)
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) < 3 or p[1] not in ("T", "t"):
                continue
            try:
                addr = int(p[0], 16)
            except ValueError:
                continue
            d = demangle(p[2])
            if d:
                rows.append((d[1], d[0], addr, p[2]))
    return rows


def main():
    ap = argparse.ArgumentParser(description="按模块/名字/地址查询符号表")
    ap.add_argument("--module", help="模块路径（默认精确匹配，加 --sub 则子串匹配）")
    ap.add_argument("--sub", action="store_true", help="模块名用子串匹配")
    ap.add_argument("--name", help="函数名子串匹配")
    ap.add_argument("--addr", help="反查地址（如 0x1403d3380）")
    ap.add_argument("--big", type=int, default=0, help="只看函数体 ≥ 该字节数")
    ap.add_argument("--limit", type=int, default=40, help="最多输出条数")
    ap.add_argument("--all", action="store_true", help="不做大小筛选，全列")
    args = ap.parse_args()

    bounds = load_bounds()

    # 地址反查模式
    if args.addr:
        va = int(args.addr, 16)
        demangle = load_demangler()
        rows = load_symbols(demangle)
        best = None
        for mod, fn, a, raw in rows:
            if a <= va and (best is None or a > best[2]):
                best = (mod, fn, a, raw)
        if not best:
            print("未找到该地址对应的函数")
            return 1
        size = bounds.get(best[2], 0)
        print(f"地址     0x{va:X}")
        print(f"函数     {best[1]}  ({best[0]})")
        print(f"起始     0x{best[2]:X}")
        print(f"大小     {size} 字节")
        print(f"mangled  {best[3]}")
        return 0

    demangle = load_demangler()
    rows = load_symbols(demangle)

    sel = []
    for mod, fn, addr, raw in rows:
        if args.module:
            ok = (args.module in mod) if args.sub else (mod == args.module)
            if not ok:
                continue
        if args.name and args.name.lower() not in fn.lower():
            continue
        size = bounds.get(addr, 0)
        if args.big and size < args.big:
            continue
        sel.append((size, fn, mod, addr))

    if not sel:
        print("无匹配")
        return 1

    sel.sort(reverse=True)
    show = sel if args.all else sel[:args.limit]
    total = len(sel)
    print(f"匹配 {total} 个函数"
          + (f"，显示前 {len(show)}（按大小降序）" if len(show) < total else ""))
    print()
    print(f"{'大小':>8}  {'地址':<14} {'模块':<34} 函数")
    print("-" * 96)
    for size, fn, mod, addr in show:
        hexa = f"0x{addr:X}"
        print(f"{size:>8}  {hexa:<14} {mod:<34} {fn}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

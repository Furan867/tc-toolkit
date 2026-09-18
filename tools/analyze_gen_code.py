#!/usr/bin/env python3
"""分析从游戏内存提取的生成代码（scan_game_mem.py 的产物），统计各组件的代码代价。

输入: work/generated_code_*.si
输出: work/component_cost.tsv  +  控制台摘要

不是所有 generated_code_*.si 都是电路代码 —— 还可能是测试脚本。
本工具只统计含 #SIMULATION_STATE 的文件，其余跳过并说明。

生成代码的结构（每个组件一段）:
    // <组件号> <组件类型> <位宽> [编码] [标签]
        ...该组件生成的语句...

用法:
    python tools/analyze_gen_code.py                     # 分析全部（自动过滤）
    python tools/analyze_gen_code.py --file <路径>
    python tools/analyze_gen_code.py --detail com_switch_bit   # 只看该类型
    python tools/analyze_gen_code.py --list              # 仅列出文件与判定
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import sys
import unicodedata
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORK = os.path.join(ROOT, "work")

HEAD_RE = re.compile(
    r"^\s*//\s*(\d+)\s+(com_\w+)\s+(-?\d+)\s+(\d*)\s*(.*?)\s*$", re.MULTILINE)


def dwidth(s):
    """显示宽度：东亚宽字符算 2。"""
    return sum(2 if unicodedata.east_asian_width(c) in ("W", "F") else 1
               for c in s)


def pad(s, width, right=False):
    n = max(0, width - dwidth(s))
    return (" " * n + s) if right else (s + " " * n)


def classify(path):
    """判断文件是不是电路代码。

    电路代码由「组件段」构成，组件注释占比高；
    test.si 测试脚本里只有零星组件注释，主体是测试逻辑。
    """
    with open(path, encoding="utf-8", errors="replace") as f:
        head = f.read(4_000_000)
    ncomp = len(HEAD_RE.findall(head))
    nstate = head.count("#SIMULATION_STATE")
    nlines = head.count("\n") or 1
    if ncomp >= 50 and ncomp / nlines > 0.02:
        return "circuit", ncomp, nstate
    return "other", ncomp, nstate


# 组件段的终止标记：文件末尾的收尾代码（仿真循环、测试脚本）会跟在最后一个
# 组件注释之后，不截断的话会被整段算进最后一个组件里。
STOP_RE = re.compile(r"^\s*(def\s|if\s+in_scope\()")


def parse(path):
    segs = []
    cur = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = HEAD_RE.match(line)
            if m and m.group(2).startswith("com_"):
                cur = (int(m.group(1)), m.group(2), m.group(3),
                       m.group(5), [])
                segs.append(cur)
                continue
            if cur is None:
                continue
            if STOP_RE.match(line):
                cur = None       # 组件段结束，后面是函数级代码
                continue
            cur[4].append(line)
    return segs


def code_lines(body):
    return sum(1 for l in body
               if l.strip() and not l.strip().startswith("//"))


def main():
    ap = argparse.ArgumentParser(description="统计生成代码中各组件的代价")
    ap.add_argument("--file", help="指定文件（默认 work/generated_code_*.si）")
    ap.add_argument("--detail", help="只显示指定组件类型（不输出整体摘要）")
    ap.add_argument("--list", action="store_true", help="只列出文件与判定")
    ap.add_argument("--top", type=int, default=25, help="摘要显示条数")
    ap.add_argument("--max-detail", type=int, default=2,
                    help="--detail 模式最多显示几个实例")
    args = ap.parse_args()

    files = ([args.file] if args.file
             else sorted(glob.glob(os.path.join(WORK, "generated_code_*.si"))))
    if not files:
        print(f"找不到生成代码文件（{WORK}/generated_code_*.si）", file=sys.stderr)
        print("请先运行 tools/scan_game_mem.py 从游戏进程提取", file=sys.stderr)
        return 1

    circuits = []
    info = []
    for f in files:
        kind, ncomp, nstate = classify(f)
        info.append((f, kind, ncomp, nstate))
        if kind == "circuit":
            circuits.append(f)

    def print_table():
        print(f"{pad('文件', 26)} {pad('判定', 12)} 组件注释  #SIMSTATE")
        print("-" * 60)
        for f, kind, ncomp, nstate in info:
            label = "电路代码" if kind == "circuit" else "非电路(跳过)"
            print(f"{pad(os.path.basename(f), 26)} {pad(label, 12)} "
                  f"{ncomp:>8}  {nstate:>9}")

    if args.list:
        print_table()
        return 0

    if not circuits:
        print("\n没有可分析的电路代码文件", file=sys.stderr)
        return 1

    # ---- --detail：只看指定类型 ----
    if args.detail:
        print()
        shown = 0
        for f in circuits:
            for cid, ctype, width, tag, body in parse(f):
                if ctype != args.detail:
                    continue
                print(f"=== {os.path.basename(f)}  #{cid}  "
                      f"宽{width}  {tag}  ({code_lines(body)} 行) ===")
                print("".join(body).rstrip())
                print()
                shown += 1
                if shown >= args.max_detail:
                    return 0
        if not shown:
            print(f"未找到类型 {args.detail}")
            return 1
        return 0

    # ---- 整体摘要 ----
    print_table()
    all_segs = []
    for f in circuits:
        all_segs += parse(f)
    if not all_segs:
        print("未解析到组件段", file=sys.stderr)
        return 1

    by_type = defaultdict(list)
    for cid, ctype, width, tag, body in all_segs:
        by_type[ctype].append((cid, width, tag, code_lines(body)))

    total_lines = sum(code_lines(s[4]) for s in all_segs)
    rows = []
    for ctype, insts in by_type.items():
        counts = [i[3] for i in insts]
        rows.append((sum(counts), len(insts), max(counts),
                     sum(counts) / len(counts), ctype))
    rows.sort(reverse=True)

    print()
    print(f"组件实例总数: {len(all_segs)}   生成代码总行数: {total_lines}")
    print()
    cols = [("总行数", 10, True), ("占比", 8, True), ("实例数", 8, True),
            ("最重", 7, True), ("平均", 8, True), ("组件类型", 30, False)]
    print(" ".join(pad(h, w, r) for h, w, r in cols))
    print("-" * (sum(w for _, w, _ in cols) + len(cols)))
    for tot, n, mx, avg, ctype in rows[:args.top]:
        vals = [f"{tot}", f"{100*tot/total_lines:.1f}%", f"{n}", f"{mx}",
                f"{avg:.1f}", ctype]
        print(" ".join(pad(v, w, r) for v, (_, w, r) in zip(vals, cols)))

    out = os.path.join(WORK, "maps", "component_cost.tsv")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# type\ttotal_lines\tinstances\tmax\tavg\n")
        for tot, n, mx, avg, ctype in rows:
            f.write(f"{ctype}\t{tot}\t{n}\t{mx}\t{avg:.1f}\n")
    print()
    print(f"输出: {out}")

    print()
    print("=== 代码最长的 10 个组件实例 ===")
    flat = sorted(all_segs, key=lambda s: -code_lines(s[4]))[:10]
    for cid, ctype, width, tag, body in flat:
        print(f"  #{cid!s:<5} {pad(ctype, 26)} 宽{width!s:<4} "
              f"{code_lines(body)!s:>4} 行  {tag[:24]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

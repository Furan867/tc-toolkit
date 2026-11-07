#!python3
"""分析 JIT 生成的机器码质量：量化编译器浪费了多少指令。

输入是一段 JIT 机器码的裸二进制 dump（从游戏内存或转发层捕获里拿到的都可以），
用 objdump 反汇编后按模式统计冗余指令。

用法
----
    python tools/perf_opt/optimizer/analyze.py <dump.bin>
    python tools/perf_opt/optimizer/analyze.py <dump.bin> --start 0x17690 --stop 0x1e5d0
    python tools/perf_opt/optimizer/analyze.py <dump.bin> --base 0x022FEADE0000

`--start` / `--stop` 都是**文件偏移**。若文件前 8 字节是长度头（小端整数，
值等于文件总长 —— `compile()` 产物的形态），默认自动跳过。
解析完会自检「偏移连续 / 零空洞 / 字节总长」，不自洽就报错退出。

统计的冗余模式
--------------
1. mov A,B 紧跟 mov B,A            —— 来回搬运，两条都是纯浪费
2. mov ecx,imm 紧跟 shl/shr x,cl   —— 移位量本可用立即数，且多一条依赖
3. shl N 紧跟 shr N（同寄存器同移位数）—— 等价于一次 and 掩码
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path
from typing import TypedDict

HERE = os.path.dirname(os.path.abspath(__file__))       # tools/perf_opt/optimizer
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))      # 仓库根
sys.path.insert(0, ROOT)

from libs import tc_common  # noqa: E402

# binutils 从 PATH 找：写死过一个 w64devkit 的绝对路径，换机器就失效
OBJDUMP = tc_common.tool("objdump")

#: objdump 的字节列上限是 7 字节，更长的指令（如 10 字节的 movabs）会把剩余
#: 字节另起一行，那一行**没有助记符**（形态 `      6f:\t7f 00 00 `）。
#: 旧正则用 `(\S+)` 抓助记符，回溯后会把行尾最后一个字节吃成「助记符 00」。
#:
#: 助记符也允许 `(bad)`：objdump 对 64 位模式下非法的单字节（实测见 `62`、`2f`）
#: 就这样输出。**必须收进来**，否则字节不连续 —— 实测某份 203480 字节的产物因此
#: 出现 41 段空洞。它们含义不明，所以改写时一律绕开（见 optimizer/core.py）。
INS_RE = re.compile(
    r"^\s+([0-9a-f]+):\t((?:[0-9a-f]{2} )+)\s*\t\s*([a-z][a-z0-9.]*|\(bad\))\s*(.*)$")
CONT_RE = re.compile(r"^\s+([0-9a-f]+):\t((?:[0-9a-f]{2} )+)\s*$")


class Stats(TypedDict):
    """stats() 的返回结构。"""

    n: int
    ping: int
    cl: int
    mask: int
    kinds: Counter[str]
    save: int


def header_len(path):
    """产物前 8 字节是长度头（小端整数，值 == 文件总长），不是机器码。"""
    size = os.path.getsize(path)
    if size < 8:
        return 0
    head = Path(path).read_bytes()[:8]
    return 8 if int.from_bytes(head, "little", signed=True) == size else 0


def parse_objdump(text):
    """objdump 输出 → [[偏移, 字节, 助记符, 操作数], ...]，续行拼回上一条。

    不拼续行的话，10 字节的 movabs 只能拿到前 7 字节 —— 偏移与长度对不上，
    将来做重定位会整体错位。
    """
    insns = []
    for line in text.splitlines():
        m = INS_RE.match(line)
        if m:
            insns.append([int(m.group(1), 16),
                          bytes.fromhex(m.group(2).replace(" ", "")),
                          m.group(3), m.group(4).split("#")[0].strip()])
            continue
        c = CONT_RE.match(line)
        if c and insns:
            insns[-1][1] += bytes.fromhex(c.group(2).replace(" ", ""))
    return insns


def check_coverage(insns, begin, end):
    """自检：(空洞数, 重叠数, 尾部缺口)。指令字节应恰好铺满 [begin, end)。"""
    holes = overlap = 0
    prev_end = begin
    for off, raw, _mn, _ops in insns:
        if off > prev_end:
            holes += 1
        elif off < prev_end:
            overlap += 1
        prev_end = off + len(raw)
    return holes, overlap, end - prev_end


def load_insns(path, start=None, stop=None):
    """反汇编并自检，返回 [[偏移, 字节, 助记符, 操作数], ...]；不自洽时返回 None。

    与 disasm() 的区别是**保留原始字节** —— 改写器要靠字节长度判断能不能等长替换。
    """
    size = os.path.getsize(path)
    head = header_len(path)
    begin = head if start is None else start
    end = size if stop is None else stop

    cmd = [OBJDUMP, "-D", "-b", "binary", "-m", "i386:x86-64", "-M", "intel",
           f"--start-address={begin}"]
    if stop is not None:
        cmd.append(f"--stop-address={stop}")
    cmd.append(path)
    out = subprocess.run(cmd, capture_output=True, text=True,
                         encoding="utf-8", errors="replace")

    parsed = parse_objdump(out.stdout)
    if not parsed:
        print("objdump 未解析出任何指令", file=sys.stderr)
        return None

    holes, overlap, tail = check_coverage(parsed, begin, end)
    if holes or overlap:
        print(f"❌ 解析不自洽：空洞 {holes} 段、重叠 {overlap} 段"
              f"（区间 0x{begin:X}..0x{end:X}）—— 结果不可信", file=sys.stderr)
        return None
    note = f"，跳过 {head} 字节长度头" if head and start is None else ""
    if tail:
        print(f"解析自检：{len(parsed)} 条指令，偏移连续、零空洞{note}；"
              f"尾部余 {tail} 字节（区间端点未对齐指令边界），"
              f"覆盖至 0x{end - tail:X}", file=sys.stderr)
    else:
        print(f"解析自检：{len(parsed)} 条指令，偏移连续、零空洞、"
              f"字节铺满 [0x{begin:X}, 0x{end:X}）{note}", file=sys.stderr)
    return parsed


def disasm(path, start, stop):
    """同 load_insns()，但去掉字节：[(偏移, 助记符, 操作数), ...]。"""
    parsed = load_insns(path, start, stop)
    if parsed is None:
        return None
    return [(off, mn, ops) for off, _raw, mn, ops in parsed]


def stats(insns) -> Stats | None:
    n = len(insns)
    if not n:
        return None
    ping = cl = mask = 0
    for i in range(n - 1):
        a, o1 = insns[i][1], insns[i][2]
        b, o2 = insns[i + 1][1], insns[i + 1][2]
        if a == "mov" and b == "mov":
            p1, p2 = o1.split(","), o2.split(",")
            if (len(p1) == 2 and len(p2) == 2
                    and p1[0] == p2[1] and p1[1] == p2[0]):
                ping += 1
        if a == "mov" and o1.startswith("ecx,") and b in ("shl", "shr", "sar") \
                and ",cl" in o2:
            cl += 1
        if a in ("shl", "shr") and b in ("shl", "shr"):
            p1, p2 = o1.split(","), o2.split(",")
            if len(p1) == 2 and len(p2) == 2 and p1 == p2:
                mask += 1
    kinds = Counter(i[1] for i in insns)
    save = ping * 2 + cl + mask
    return {"n": n, "ping": ping, "cl": cl, "mask": mask,
            "kinds": kinds, "save": save}


def main():
    ap = argparse.ArgumentParser(description="分析 JIT 机器码质量")
    ap.add_argument("file", help="dump 出来的裸二进制")
    ap.add_argument("--start", type=lambda x: int(x, 0), help="起始偏移")
    ap.add_argument("--stop", type=lambda x: int(x, 0), help="结束偏移")
    ap.add_argument("--base", type=lambda x: int(x, 0), help="区基址（仅用于显示）")
    ap.add_argument("--top", type=int, default=12, help="指令类型显示条数")
    args = ap.parse_args()

    if not os.path.isfile(args.file):
        print(f"找不到 {args.file}", file=sys.stderr)
        return 1

    insns = disasm(args.file, args.start, args.stop)
    if insns is None:
        return 2
    rng = ""
    if args.start is not None:
        rng = f"  范围 0x{args.start:X}"
        if args.base:
            rng += f" (VA 0x{args.base + args.start:X})"
        if args.stop is not None:
            rng += f" - 0x{args.stop:X}"
    print(f"{os.path.basename(args.file)}{rng}")
    s = stats(insns)
    if not s:
        print("未解析到指令")
        return 1
    n = s["n"]
    print(f"指令总数: {n}")
    print()
    print("冗余模式")
    print("-" * 58)
    print(f"  mov A,B + mov B,A 来回搬运  : {s['ping']:>5} 对   "
          f"可省 {s['ping']*2:>5} 条 ({100*s['ping']*2/n:.1f}%)")
    print(f"  mov ecx,imm + 移位用 cl     : {s['cl']:>5} 处   "
          f"可省 {s['cl']:>5} 条 ({100*s['cl']/n:.1f}%)")
    print(f"  shl N + shr N (等于 and)    : {s['mask']:>5} 对   "
          f"可省 {s['mask']:>5} 条 ({100*s['mask']/n:.1f}%)")
    print(f"  {'合计可省':<26} : {s['save']:>5} 条 ({100*s['save']/n:.1f}%)")
    print()
    print(f"指令类型 Top {args.top}")
    print("-" * 58)
    for k, v in s["kinds"].most_common(args.top):
        print(f"  {k:<12} {v:>6}  {100*v/n:>5.1f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())

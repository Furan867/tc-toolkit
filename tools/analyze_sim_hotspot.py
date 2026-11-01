#!python3
"""组件级仿真性能剖析 —— 把 RIP 采样落到电路组件上。

输入：
    --hist   sample_threads.py --hist 产出的 RIP 直方图（TSV: rip / count）
    --map    build_component_map.py 产出的 component_map.tsv

做法：
    1. 枚举游戏进程里「非 IMAGE 且可执行」的区（JIT 代码区）
    2. 每个采样地址 RIP → 找到所属区 → 偏移 = RIP − 区基址
    3. 偏移落在映射表里哪个组件的区间 → 该组件计数 +1
    4. 按组件类型汇总占比

JIT 区里可能有同一份代码的多份副本，用「相对偏移」映射会自动合并。

用法：
    python tools/sample_threads.py --iterations 2000 --hist work/rips.tsv
    python tools/analyze_sim_hotspot.py --hist work/rips.tsv
"""
from __future__ import annotations

import argparse
import bisect
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
from collections import Counter

# Windows 下 stdout 走管道时默认 GBK，输出的中文会变成乱码
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(HERE)
WORK = os.path.join(ROOT, "work")

import tc_common  # noqa: E402
from term import field, paint  # noqa: E402

MEM_COMMIT = 0x1000
MEM_IMAGE = 0x1000000
EXEC_MASK = 0x10 | 0x20 | 0x40 | 0x80
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010

GAME_EXE = tc_common.GAME_EXE_NAME


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("_a", wt.DWORD),
                ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD),
                ("Protect", wt.DWORD), ("Type", wt.DWORD), ("_b", wt.DWORD)]


def find_pid(name=GAME_EXE):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout.strip()
    if not out.startswith('"'):
        return None
    try:
        return int(out.split('","')[1])
    except (IndexError, ValueError):
        return None


def exec_regions(pid):
    """枚举「非 IMAGE 且可执行」的区，返回按基址排序的 [(base, size)]。"""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p,
                                   ctypes.POINTER(MBI), ctypes.c_size_t]
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        raise RuntimeError(f"OpenProcess({pid}) 失败")
    out, addr = [], 0
    while addr < 0x7FFFFFFF0000:
        m = MBI()
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(m), ctypes.sizeof(m)):
            break
        if m.State == MEM_COMMIT and m.Type != MEM_IMAGE and (m.Protect & EXEC_MASK):
            out.append((m.BaseAddress or 0, m.RegionSize))
        addr = (m.BaseAddress or 0) + m.RegionSize
    out.sort()
    return out


def dw(s):
    """字符串显示宽度：CJK 字符占 2 列（否则中文表头会和数字列错位）。"""
    return sum(2 if ord(c) > 0x2E80 else 1 for c in s)


def cell(s, width, right=False):
    """按显示宽度对齐的单元格。"""
    pad = " " * max(0, width - dw(s))
    return pad + s if right else s + pad


def main():
    ap = argparse.ArgumentParser(description="组件级仿真性能剖析")
    ap.add_argument("--hist", required=True, help="RIP 直方图（sample_threads.py --hist 产物）")
    ap.add_argument("--map", dest="mapfile", default=os.path.join(WORK, "maps", "component_map.tsv"))
    ap.add_argument("--pid", type=int)
    ap.add_argument("--top", type=int, default=0,
                    help="只列前 N 个组件类型；0（默认）= 全部")
    ap.add_argument("--instances", type=int, default=10,
                    help="组件实例：列采样最多的前 N 个（默认 10）；0 = 不输出")
    args = ap.parse_args()

    rows = []
    for line in open(args.mapfile, encoding="utf-8").read().splitlines()[1:]:
        p = line.split("\t")
        if len(p) >= 8:
            # 列：seg / comp_index / comp_id / type / src_line / jit_start / jit_end / size
            # 丢掉 seg：**一行是一段机器码**，同一个组件的多段共享 comp_index，
            # 聚合靠 comp_index（否则组件数会翻倍，见 docs/facts.md）。
            rows.append((int(p[1]), int(p[2]), p[3], int(p[4]), int(p[5]), int(p[6])))
    if not rows:
        print("映射表为空", file=sys.stderr)
        return 1
    starts = [r[4] for r in rows]
    map_len = max(r[5] for r in rows)

    hist = []
    for line in open(args.hist, encoding="utf-8").read().splitlines()[1:]:
        p = line.split("\t")
        if len(p) == 2:
            hist.append((int(p[0]), int(p[1])))
    total = sum(n for _, n in hist)

    pid = args.pid or find_pid()
    if not pid:
        print("未找到游戏进程（可用 --pid 指定）", file=sys.stderr)
        return 1
    regions = exec_regions(pid)
    bases = [b for b, _ in regions]

    per_type, per_inst = Counter(), Counter()
    in_jit = beyond = outside = 0
    for rip, n in hist:
        i = bisect.bisect_right(bases, rip) - 1
        off = None
        if i >= 0:
            b, sz = regions[i]
            if b <= rip < b + sz:
                off = rip - b
        if off is None:
            outside += n
            continue
        if off >= map_len:
            beyond += n
            continue
        j = bisect.bisect_right(starts, off) - 1
        if j >= 0 and rows[j][4] <= off < rows[j][5]:
            per_type[rows[j][2]] += n
            per_inst[rows[j][0]] += n
            in_jit += n
        else:
            beyond += n

    print(f"采样总点数 {field('count', str(total))}；"
          f"JIT 区 {field('n', str(len(regions)))} 个，"
          f"组件映射 {field('n', str(len(rows)))} 个 / {field('bytes', str(map_len))} 字节")
    print(f"  命中组件      : {field('count', str(in_jit))} "
          f"({field('pct', format(100 * in_jit / max(1, total), '.1f') + '%')})")
    print(f"  JIT 区但未命中（固定前缀、无代码组件）: {field('dim', str(beyond))}")
    print(f"  不在 JIT 区（系统 dll / 其他线程）   : {field('dim', str(outside))}")
    if in_jit == 0:
        print("\n没有采样点落在组件代码上 —— 电路可能没在运行，或采样时它处于空闲。")
        return 0
    # 按 comp_index 聚合到**组件**级：一个组件可能有多段机器码（源码里两条注释，
    # 见 build_component_map 的说明）。不聚合的话组件数会翻倍（实测 1234 段 = 617 个）。
    comp_type, comp_line, comp_size = {}, {}, Counter()
    for r in rows:
        comp_type.setdefault(r[0], r[2])      # 组件 -> 类型
        comp_line.setdefault(r[0], r[3])      # 组件 -> 首次出现的源码行
        comp_size[r[0]] += r[5] - r[4]        # 组件 -> 各段机器码字节之和
    inst_count = Counter(comp_type.values())

    print()
    n_type = len(inst_count) if args.top <= 0 else min(args.top, len(inst_count))
    print(paint(f"=== 组件类型 → 采样占比（共 {n_type} 种）===", "bold"))
    # 表头与数据列用**同一套 field() 配色**，列与表头一眼对应；print 的多参数
    # 会自动补列间空格（改成字符串拼接会丢掉它们，表头挤成「占比组件数」）
    print(field("count", cell("采样数", 8, True)),
          field("pct", cell("占比", 7, True)),
          field("n", cell("组件数", 6, True)),
          field("bytes", cell("平均字节", 9, True)),
          field("name", "组件类型"))
    print("-" * 64)
    # ⚠️ 以**映射表**为准列出全部类型，而不是只列采到的 —— 否则采样为 0 的类型会
    # 整行消失（如 com_cc_input / com_cc_output），看起来像"电路里没有这些组件"。
    # 实测就因此被误问过一次。
    order = sorted(inst_count, key=lambda t: (-per_type.get(t, 0), t))
    if args.top > 0:
        order = order[:args.top]
    for t in order:
        n = per_type.get(t, 0)
        pct = 100 * n / in_jit
        tot = sum(sz for c, sz in comp_size.items() if comp_type[c] == t)
        avg = tot / max(1, inst_count[t])
        # 一律先 cell() 对齐再 field() 上色 —— 反过来的话 ANSI 转义会被算进宽度
        print(field("count", cell(str(n), 8, True)),
              field("pct", cell(f"{pct:.1f}%", 7, True)),
              field("n", cell(str(inst_count[t]), 6, True)),
              field("bytes", cell(f"{avg:.0f}", 9, True)),
              field("name", t))

    if args.instances != 0:
        print()
        # 只列最热的 N 个 —— 组件可能有几百个，全列会淹掉上面那张表。
        # 与类型表不同：类型表要列**全部**（否则零采样的类型会消失，看起来像不存在），
        # 实例表则是按热度排序的，截断后"看不到"只是表示它不热。
        all_comp = sorted(comp_type, key=lambda c: -per_inst.get(c, 0))
        all_comp = all_comp[:args.instances]
        print(paint(f"=== 最热的 {len(all_comp)} 个组件实例"
                    f"（全部 {len(comp_type)} 个，用 --instances 调整）===", "bold"))
        for c in all_comp:
            n = per_inst.get(c, 0)
            pct = 100 * n / in_jit
            print(f"  {field('n', '组件#%-4d' % c)} {field('name', '%-24s' % comp_type[c])} "
                  f"{field('dim', '源码行%-6d' % comp_line[c])} "
                  f"{field('count', format(n, '>7'))} 点 "
                  f"{field('pct', format(pct, '.1f') + '%')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

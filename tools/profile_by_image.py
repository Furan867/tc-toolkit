#!/usr/bin/env python3
"""一键剖析：定位产物 → 采样 → 按产物字节范围过滤 → 组件报告。

为什么需要「按产物范围过滤」
--------------------------
`analyze_sim_hotspot.py` 自带判据是「RIP 落在非 IMAGE 的可执行区」—— 太粗，
会把**旧电路残留**和**其它用途的 JIT 代码**一起算进来。实测同一份机器码在内存里
有好几处「特征命中但整段不符」的副本，靠区判据根本分不开。

有了产物和它的加载地址之后，判据可以精确到字节：

    base <= RIP < base + len(机器码)      ← 只有在跑**这一份**电路

而且实测产物就是从 JIT 区基址开始加载的，所以 `offset = RIP - base` 与
`analyze_sim_hotspot.py` 内部算法一致，过滤后的直方图可以直接喂给它。

用法
----
    python tools/profile_by_image.py --src work/hook/hook_src_003.si --img work/hook/hook_out_004.bin
    python tools/profile_by_image.py --src ... --img ... --duration 20
"""
from __future__ import annotations

import argparse
import collections
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORK = os.path.join(ROOT, "work")
PY = sys.executable
sys.path.insert(0, HERE)

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

from winmem import k32, find_pid, open_process
from locate_jit_image import locate  # noqa: E402

TAB = chr(9)
NL = chr(10)


def load_body(path):
    """读产物文件，剥掉前 8 字节长度头，返回机器码。

    ⚠️ 头字段的值等于**文件总长（含头自己）**，不是数据长度 —— 按
    「head == len(img) - 8」判断会判不出来，带着头去搜会让特征偏移整体错 8 字节。
    """
    img = open(path, "rb").read()
    head = struct.unpack("<q", img[:8])[0] if len(img) >= 8 else -1
    return img[8:] if head == len(img) else img


def run(args):
    # 先 flush：输出走管道时父进程 stdout 是块缓冲，不 flush 的话子进程的输出会
    # 插到我们已经 print 但还没落盘的内容前面，看起来顺序错乱
    sys.stdout.flush()
    return subprocess.run(args, cwd=ROOT)


def discover_pairs():
    """扫 work/hook/ 下的 src/out 对，**按捕获时间升序**返回 [(编号, src, out)]。

    配对：**同编号即一对** —— 转发层一次 compile() 调用取一个号，src/out 共用，
    所以 hook_src_005.si ↔ hook_out_005.bin。

    ⚠️ **排序绝不能靠编号**：计数器是**进程内**变量，游戏一重启就归零，目录里于是
    会出现「旧的 src_089 与新的 src_001 并存」。按编号取最大就选中了**旧进程的产物**，
    它的 JIT 区早随进程消失 → 定位 0 候选（实测踩到，表现成「工具坏了」）。故按 mtime。
    同理，`--watch` 里判断「是否见过」要用**文件路径**而不是编号。
    """
    import glob
    srcs, outs = {}, {}
    for f in glob.glob(os.path.join(WORK, "hook", "hook_src_*")):
        m = re.search(r"hook_src_(\d+)\.(?:si|bin)$", f)
        if m:
            srcs[int(m.group(1))] = f
    for f in glob.glob(os.path.join(WORK, "hook", "hook_out_*.bin")):
        m = re.search(r"hook_out_(\d+)\.bin$", f)
        if m:
            outs[int(m.group(1))] = f
    pairs = [(seq, srcs[seq], outs[seq]) for seq in (srcs.keys() & outs.keys())]
    pairs.sort(key=lambda p: os.path.getmtime(p[2]))
    return pairs



def component_inventory(src_path):
    """数出各类型组件的数量（按唯一 ID 计）。

    ⚠️ 完整源码里每个组件 ID 会出现**两次**（组件代码 + 绑定），所以必须按唯一
    ID 去重 —— 直接数注释条数会翻倍（127 个 nand 数成 254，这个坑早先误杀过
    全部正常候选）。同时返回源码自报的 get_component_count() 供对照。
    """
    s = open(src_path, encoding="utf-8", errors="replace").read()
    seen = {}
    for cid, name in re.findall(r"//\s*(\d+)\s+(com_[a-z0-9_]+)", s):
        if name == "com_none":
            continue          # 空注释，不是真组件
        seen.setdefault(cid, name)
    m = re.search(r"get_component_count\(\)\s*Int\s*\{\s*return\s+(\d+)", s)
    return collections.Counter(seen.values()), (int(m.group(1)) if m else None)


def report_one(seq, src, out):
    inv, declared = component_inventory(src)
    total = sum(inv.values())
    print("── src_%03d / out_%03d   捕获于 %s ──"
          % (seq, seq, time.strftime("%m-%d %H:%M:%S",
                                     time.localtime(os.path.getmtime(out)))))
    if not inv:
        print("   （源码里没有组件注释）")
    else:
        width = max(len(n) for n in inv)
        for name, cnt in inv.most_common():
            print("   %-*s  x %d" % (width, name, cnt))
        print("   " + "-" * (width + 8))
    print("   合计 %d 个（源码自报 get_component_count() = %s）" % (total, declared))
    print()



def watch(args):
    print("监听 work/ 下新出现的 src/out 对（Ctrl-C 停止）")
    print("每发现一对就报告组件构成；**不做性能分析**。" + NL)
    seen = {out for _, _, out in discover_pairs()}
    print("（已有 %d 对，从现在起只报新出现的）" % len(seen) + NL)
    try:
        while True:
            for seq, src, out in discover_pairs():
                if out in seen:          # 用**路径**判重，不用编号（编号会因重启归零）
                    continue
                seen.add(out)
                report_one(seq, src, out)
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print(NL + "停止监听。")
    return 0


def main():
    ap = argparse.ArgumentParser(description="按产物字节范围做组件级性能剖析")
    ap.add_argument("--src", help="Simplex 源码（hook_src_*.si）")
    ap.add_argument("--img", help="编译产物（hook_out_*.bin）")
    ap.add_argument("--duration", type=float, default=10.0, help="采样秒数")
    ap.add_argument("--pid", type=int)
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--watch", action="store_true",
                    help="监听模式：自动发现新出现的 src/out 对并报告组件构成，"
                         "不做性能分析（用于人工验证「最新 ?= 正在跑」）")
    ap.add_argument("--interval", type=float, default=0.02, help="监听轮询间隔秒")
    args = ap.parse_args()

    if args.watch:
        return watch(args)

    # 子进程统一以 reversing/ 为工作目录运行，所以路径必须先转成绝对路径 ——
    # 否则 'reversing/work/x.bin' 会被拼成 'reversing/reversing/work/x.bin'（踩过）
    if not args.src or not args.img:
        # 默认用**最新的** src/out 对 —— 它就是当前最新电路状态
        pairs = discover_pairs()
        if not pairs:
            print("❌ work/hook/ 下没有任何 src/out 对", file=sys.stderr)
            return 1
        seq, args.src, args.img = pairs[-1]
        print("使用最新的 src/out 对：src_%03d / out_%03d（捕获于 %s）"
              % (seq, seq, time.strftime("%m-%d %H:%M:%S",
                                         time.localtime(os.path.getmtime(args.img)))))
    args.src = os.path.abspath(args.src)
    args.img = os.path.abspath(args.img)

    # ── 1. 定位产物的加载地址 ──
    body = load_body(args.img)
    pid = args.pid or find_pid()
    if not pid:
        print("❌ 游戏没在运行", file=sys.stderr)
        return 1
    print(f"[1/4] 定位产物（{len(body)} 字节机器码）在 PID {pid} 里的加载地址 ...")
    h = open_process(pid)
    if not h:
        print("❌ OpenProcess 失败", file=sys.stderr)
        return 1
    try:
        hits = locate(h, body, want_all=True)
    finally:
        k32.CloseHandle(h)
    # ⚠️ 同一份机器码在内存里**可能有多个副本** —— 每编译一次就占一个新的 JIT 区，
    #    而旧区不释放。locate() 返回的第一个不一定是在跑的那个（实测：31 次编译
    #    同一电路，31 个相同产物落在不同区）。所以全部定位出来，等采样之后再按
    #    「哪个副本真的收到 RIP」来筛。
    bases = [addr for addr, verified, _ in hits if verified]
    if not bases:
        if not hits:
            print("❌ 这份产物**根本不在内存里**（特征 0 次命中）—— 多半是游戏重启前"
                  "捕获的，JIT 区已随进程消失")
        else:
            print(f"❌ 没找到这份机器码（{len(hits)} 个候选全部未通过整段校验）")
        print("   电路跑的可能不是这份产物 —— 用 --img 指定别的 hook_out_*.bin")
        return 1
    print("      → 找到 %d 个副本：%s" % (len(bases), ", ".join("0x%X" % b for b in bases)))

    # ── 2. 采样全部线程 ──
    raw = os.path.join(WORK, "tmp", "_prof_raw.tsv")
    print(f"[2/4] 采样 {args.duration:.0f} 秒 ...")
    if find_pid() != pid:                     # 采样前确认进程没变
        print("❌ 游戏进程变了，地址已失效 —— 重新跑一次", file=sys.stderr)
        return 1
    # 把产物的字节范围交给采样器：它会先探测、锁定真正在跑这份机器码的线程，
    # 再只采这些线程 —— 用精确判据筛选，而不是旧版「RIP 落在非 IMAGE 可执行区」
    # 那种会把旧电路残留一起算进来的粗判据。
    cmd = [PY, os.path.join(HERE, "sample_threads.py"),
           "--duration", str(args.duration), "--hist", raw]
    for b in bases:
        cmd += ["--target", "0x%X:%d" % (b, len(body))]
    run(cmd)
    if not os.path.exists(raw):
        print("❌ 采样没有产出直方图", file=sys.stderr)
        return 1

    # ── 3. 按产物字节范围过滤，并筛出「正在跑的那个副本」──
    kept = os.path.join(WORK, "tmp", "_prof_filtered.tsv")
    samples = []
    for line in open(raw, encoding="utf-8").read().splitlines()[1:]:
        q = line.split(TAB)
        if len(q) != 2:
            continue
        try:
            samples.append((int(q[0]), int(q[1])))
        except ValueError:
            continue
    total = sum(c for _, c in samples)

    per_copy = {}
    for rip, cnt in samples:
        for b in bases:
            if b <= rip < b + len(body):
                per_copy[b] = per_copy.get(b, 0) + cnt
    if not per_copy:
        print(f"[3/4] ❌ {total} 个采样点没有一个落在这份机器码的任何副本上")
        print("      电路没在运行，或跑的是别的电路 —— 换一份 hook_out_*.bin 试试")
        return 1
    base = max(per_copy, key=per_copy.get)
    inside = per_copy[base]
    others = {b: n for b, n in per_copy.items() if b != base}
    print(f"[3/4] 过滤：{total} 个采样点中 {inside} 个落在该机器码内"
          f"（{100.0 * inside / max(total, 1):.2f}%）")
    print(f"      正在跑的是副本 0x{base:X}"
          + (f"；另有 {len(others)} 个副本也收到采样 {sum(others.values())} 次"
             if others else ""))
    with open(kept, "w", encoding="utf-8", newline=NL) as g:
        g.write("rip" + TAB + "count" + NL)
        for rip, cnt in samples:
            if base <= rip < base + len(body):
                g.write("%d%s%d%s" % (rip, TAB, cnt, NL))

    # ── 4. 建映射 + 出报告 ──
    print("[4/4] 建组件映射 + 出报告 ...")
    run([PY, os.path.join(HERE, "build_component_map.py"),
         "--src", args.src, "--keep"])
    run([PY, os.path.join(HERE, "analyze_sim_hotspot.py"),
         "--hist", kept, "--top", str(args.top)])
    return 0


if __name__ == "__main__":
    sys.exit(main())

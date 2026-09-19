#!/usr/bin/env python3
"""采样 Turing Complete 各线程的指令指针（RIP），做出「时间花在哪」的分布。

原理
----
周期性对目标线程 SuspendThread → GetThreadContext 读 RIP → ResumeThread，
把采样到的地址按所属模块归类。采样次数越多，分布越接近真实热点。
暂停时间在微秒级，对游戏基本无感。

用途
----
- 判断电路仿真的代码到底跑在哪：exe/dll 里（解释执行），
  还是动态分配的内存里（JIT 生成的机器码）
- 找出最吃 CPU 的线程

只读，不修改目标进程。
"""
from __future__ import annotations

import argparse
import ctypes
import struct
import sys
import time
from collections import Counter
from ctypes import wintypes

# Windows 下 stdout 走管道时默认 GBK，输出的中文/符号会变成乱码（被
# profile_by_image.py 当子进程调用时必然走管道）—— 统一转成 UTF-8
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

HERE_PID_HINT = "Turing Complete.exe"

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

TH32CS_SNAPTHREAD = 0x4
THREAD_GET_CONTEXT = 0x0008
THREAD_QUERY_INFORMATION = 0x0040
THREAD_SUSPEND_RESUME = 0x0002
CONTEXT_CONTROL = 0x00100001
CONTEXT_SIZE = 1232          # x64 CONTEXT 结构大小
RIP_OFFSET = 0xF8            # 其中 Rip 字段的偏移
PROBE_ROUNDS = 40            # 探测「哪个线程在跑目标代码」的粗采轮数


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                ("th32ThreadID", wintypes.DWORD),
                ("th32OwnerProcessID", wintypes.DWORD),
                ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
                ("dwFlags", wintypes.DWORD)]


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_ulonglong),
                ("AllocationBase", ctypes.c_ulonglong),
                ("AllocationProtect", wintypes.DWORD),
                ("PartitionId", wintypes.WORD),
                ("RegionSize", ctypes.c_ulonglong),
                ("State", wintypes.DWORD),
                ("Protect", wintypes.DWORD),
                ("Type", wintypes.DWORD)]


TY = {0x20000: "PRIVATE", 0x40000: "MAPPED", 0x1000000: "IMAGE"}


def find_pid(name=HERE_PID_HINT):
    TH32CS_SNAPPROCESS = 0x2

    class PE32(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
                    ("th32ModuleID", wintypes.DWORD),
                    ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD),
                    ("pcPriClassBase", ctypes.c_long),
                    ("dwFlags", wintypes.DWORD),
                    ("szExeFile", ctypes.c_char * 260)]
    k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PE32()
    e.dwSize = ctypes.sizeof(PE32)
    k32.Process32First(snap, ctypes.byref(e))
    try:
        while True:
            if e.szExeFile.decode("mbcs", "replace").lower() == name.lower():
                return e.th32ProcessID
            if not k32.Process32Next(snap, ctypes.byref(e)):
                return None
    finally:
        k32.CloseHandle(snap)


k32.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.OpenThread.restype = wintypes.HANDLE
k32.SuspendThread.argtypes = [wintypes.HANDLE]
k32.SuspendThread.restype = wintypes.DWORD
k32.ResumeThread.argtypes = [wintypes.HANDLE]
k32.ResumeThread.restype = wintypes.DWORD
k32.GetThreadContext.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
k32.GetThreadContext.restype = wintypes.BOOL
k32.VirtualQueryEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                               ctypes.POINTER(MEMORY_BASIC_INFORMATION),
                               ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t


def list_threads(pid):
    k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    if snap == wintypes.HANDLE(-1).value:
        return []
    e = THREADENTRY32()
    e.dwSize = ctypes.sizeof(THREADENTRY32)
    out = []
    try:
        ok = k32.Thread32First(snap, ctypes.byref(e))
        while ok:
            if e.th32OwnerProcessID == pid:
                out.append(e.th32ThreadID)
            ok = k32.Thread32Next(snap, ctypes.byref(e))
    finally:
        k32.CloseHandle(snap)
    return out


def read_rip(h_thread):
    buf = ctypes.create_string_buffer(CONTEXT_SIZE)
    ctypes.memset(buf, 0, CONTEXT_SIZE)
    struct.pack_into("<I", buf, 0x30, CONTEXT_CONTROL)   # ContextFlags
    if not k32.GetThreadContext(h_thread, buf):
        return None
    return struct.unpack_from("<Q", buf, RIP_OFFSET)[0]


def probe_threads(h, hts, ranges, rounds=PROBE_ROUNDS, interval=0.0005):
    """粗采若干轮，挑出「RIP 曾落在**目标机器码字节范围内**」的线程。

    其余线程都泡在 ntdll 等待 —— 锁定仿真线程后，采样开销从 70 个线程降到 1~2 个，
    同样时长能拿到几十倍的样本，对游戏的干扰也小得多。

    ⚠️ 判据是**目标产物的字节范围**，不是「非 IMAGE 可执行区」（旧判据已删）。
    旧判据会把任何 JIT 代码都算进来 —— 旧电路残留、其它用途的 JIT 都命中，
    所以只能"猜"线程、还得靠"过半命中"压制误判。字节范围没有假阳性：
    别的线程不可能跑进这份机器码里。也因此命中**一次**即可锁定 ——
    真仿真线程偶尔跑出范围跑进公共代码是无所谓的。

    ranges = [(base, len), ...]，可给多个副本（同一机器码在内存里有多份）。
    返回 Counter{tid: 命中次数}；空 = 没有任何线程在跑这些代码。
    """
    hit = Counter()
    for _ in range(rounds):
        for tid, ht in hts.items():
            if k32.SuspendThread(ht) == 0xFFFFFFFF:
                continue
            rip = read_rip(ht)
            k32.ResumeThread(ht)
            if rip and any(b <= rip < b + n for b, n in ranges):
                hit[tid] += 1
        time.sleep(interval)
    return hit


def _progress(done, total, samples, mode="auto", width=26):
    """在 stderr 上画一行进度条（`\\r` 原地刷新）。

    写 stderr 而不是 stdout —— stdout 是数据流（直方图要落到文件），进度条不该混进去。
    默认只在**终端**里画：输出被重定向到文件时完全静默，免得日志全是回车符。
    """
    if mode == "never":
        return
    if mode == "auto" and not sys.stderr.isatty():
        return
    pct = min(1.0, done / total) if total else 0.0
    fill = int(pct * width)
    sys.stderr.write("\r  [%s%s] %5.1f%%  %5.1f/%.0fs  样本 %d   "
                     % ("#" * fill, "-" * (width - fill), pct * 100,
                        done, total, samples))
    sys.stderr.flush()


def _progress_end(mode="auto"):
    """收尾：把进度条那行擦掉。"""
    if mode == "never" or (mode == "auto" and not sys.stderr.isatty()):
        return
    sys.stderr.write("\r" + " " * 74 + "\r")
    sys.stderr.flush()


def parse_ranges(items):
    """把 --target 的 'BASE:LEN'（允许 0x 前缀）解析成 [(base, len)]。"""
    out = []
    for s in items or []:
        b, sep, n = s.partition(":")
        if not sep:
            raise SystemExit(f"--target 的格式应为 BASE:LEN，收到 {s!r}")
        out.append((int(b, 0), int(n, 0)))
    return out


def sample(pid, iterations, interval, auto=True, only_tids=None, duration=None,
           target_ranges=None, progress="auto"):
    h = k32.OpenProcess(0x0400 | 0x0010, False, pid)     # QUERY_INFO | VM_READ
    tids = list_threads(pid)
    hts = {}
    for tid in tids:
        ht = k32.OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION |
                            THREAD_SUSPEND_RESUME, False, tid)
        if ht:
            hts[tid] = ht

    target = hts
    if only_tids:
        target = {t: ht for t, ht in hts.items() if t in only_tids}
    elif auto and target_ranges:
        hit = probe_threads(h, hts, target_ranges)
        if hit:
            target = {t: ht for t, ht in hts.items() if t in hit}
            print(f"  锁定 {len(hit)}/{len(hts)} 个线程在跑目标代码"
                  f"（探测 {PROBE_ROUNDS} 轮）")
        else:
            # 探测失败**不**当成错误退出：让采样照常进行，由上层按字节范围
            # 过滤并给出更准确的诊断（"没有一个采样点落在机器码内"）。
            print(f"  ⚠ 探测 {PROBE_ROUNDS} 轮，没有任何线程落在目标机器码范围内 "
                  f"—— 电路没在跑？退回采样全部线程", file=sys.stderr)

    hits = Counter()
    per_thread = {}
    misses = 0
    nsamples = 0
    # 按轮数或按时长采样。实测每次 Suspend/Resume 约 0.45ms，比 interval 大得多，
    # 所以「按时长」才是可控的口径 —— 按轮数换算不出稳定的耗时。
    deadline = time.time() + duration if duration else None
    t0 = time.time()
    n = 0
    last_draw = 0.0
    while True:
        now = time.time()
        if deadline is not None:
            if now >= deadline:
                break
        elif n >= iterations:
            break
        n += 1
        for tid, ht in target.items():
            if k32.SuspendThread(ht) == 0xFFFFFFFF:
                continue
            rip = read_rip(ht)
            k32.ResumeThread(ht)
            if rip:
                hits[rip] += 1
                per_thread.setdefault(tid, Counter())[rip] += 1
                nsamples += 1
            else:
                misses += 1
        time.sleep(interval)
        # 进度条按**时间**节流：锁定线程后每秒上千轮，按轮数刷会把终端刷爆。
        # 只在按时长采样时画 —— 按轮数采样的总进度没有时间轴可依。
        if deadline is not None:
            now = time.time()
            if now - last_draw >= 0.1:
                last_draw = now
                _progress(now - t0, duration, nsamples, progress)
    if deadline is not None:
        _progress(duration, duration, nsamples, progress)
        _progress_end(progress)

    # 把每个 RIP 归类到内存区
    region_of = {}
    def classify(addr):
        if addr in region_of:
            return region_of[addr]
        mbi = MEMORY_BASIC_INFORMATION()
        k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi),
                           ctypes.sizeof(mbi))
        res = (mbi.AllocationBase, TY.get(mbi.Type, hex(mbi.Type)))
        region_of[addr] = res
        return res

    agg = Counter()
    for rip, n in hits.items():
        base, ty = classify(rip)
        agg[(base, ty)] += n
    k32.CloseHandle(h)
    for ht in hts.values():
        k32.CloseHandle(ht)
    return hits, agg, len(target), misses, per_thread, region_of


def main():
    ap = argparse.ArgumentParser(description="采样游戏线程 RIP")
    ap.add_argument("--pid", type=int)
    ap.add_argument("--duration", type=float, default=10.0,
                    help="采样时长秒（默认 10；设 0 则改用 --iterations 计轮数）")
    ap.add_argument("--iterations", type=int, default=100000,
                    help="采样轮数（仅当 --duration 为 0 时生效）")
    ap.add_argument("--interval", type=float, default=0.0001,
                    help="轮间隔秒（Suspend/Resume 本身约 0.45ms，调它影响有限）")
    ap.add_argument("--hist", help="把完整 RIP 直方图写到该文件（TSV: rip / count）")
    ap.add_argument("--target", action="append", metavar="BASE:LEN",
                    help="目标机器码在内存里的字节范围（可重复，用于多副本）。"
                         "给了它就**先探测、锁定真正在跑这份代码的线程，再只采它们**"
                         "—— 同样时长能拿到几十倍样本，对游戏的干扰也小得多。"
                         "由 profile_by_image.py 自动传入")
    ap.add_argument("--all-threads", action="store_true",
                    help="强制采样全部线程（覆盖 --target 的锁定）")
    ap.add_argument("--tid", type=int, action="append",
                    help="只采样指定线程 ID（可重复；给了就不探测）")
    ap.add_argument("--verbose", "-v", action="store_true",
                    help="额外输出诊断：各线程分布 / RIP 区分布 / 热点地址。"
                         "被 profile_by_image.py 调用时不加")
    ap.add_argument("--progress", choices=("auto", "always", "never"), default="auto",
                    help="采样进度条：auto=只在终端里显示（默认，重定向到文件时自动"
                         "关闭）；always / never 强制开关")
    args = ap.parse_args()

    pid = args.pid or find_pid()
    if not pid:
        print("未找到游戏进程", file=sys.stderr)
        return 1
    print(f"PID {pid}，采样 {args.duration:.1f} 秒 ...")
    hits, agg, nthreads, misses, per_thread, region_of = sample(
        pid, args.iterations, args.interval,
        auto=not args.all_threads, only_tids=set(args.tid) if args.tid else None,
        duration=args.duration or None, target_ranges=parse_ranges(args.target),
        progress=args.progress)
    print(f"  可采样线程 {nthreads} 个，读到 RIP {sum(hits.values())} 次，"
          f"失败 {misses} 次")
    if args.hist:
        with open(args.hist, "w", encoding="utf-8", newline="\n") as f:
            f.write("rip\tcount\n")
            for rip, n in sorted(hits.items()):
                f.write(f"{rip}\t{n}\n")
        print(f"  直方图 {len(hits)} 个不同地址 → {args.hist}")

    if not args.verbose:          # 以下都是诊断，平时不输出
        return 0
    print()
    print("=== 各线程的活动分布（只列有非 IMAGE 活动的线程）===")
    rows_t = []
    for tid, cnt in per_thread.items():
        nonimg = sum(n for r, n in cnt.items()
                     if region_of.get(r, (0, "IMAGE"))[1] != "IMAGE")
        if nonimg:
            rows_t.append((tid, sum(cnt.values()), nonimg, cnt))
    for tid, tot_t, nonimg, cnt in sorted(rows_t, key=lambda x: -x[2]):
        top = cnt.most_common(3)
        summary = "  ".join(f"0x{r:X}x{n}" for r, n in top)
        print(f"  TID {tid:<7} 采样 {tot_t:>5}  其中非 IMAGE {nonimg:>5}  |  {summary}")
    if not rows_t:
        print("  （无 —— 所有线程都泡在系统 dll 里）")
    print()
    print("=== RIP 按内存区分布（Top 12）===")
    print(f"{'采样数':>8} {'占比':>7}  {'区基址':<16} 类型    首个命中地址")
    print("-" * 72)
    tot = sum(agg.values()) or 1
    for (base, ty), n in agg.most_common(12):
        print(f"{n:>8} {100*n/tot:>6.1f}%  0x{base:012X}  {ty:<8}")
    non_image = sum(n for (b, t), n in agg.items() if t != "IMAGE")
    print()
    print(f"  非 IMAGE 区（JIT 生成的代码）活动: {non_image}/{tot} "
          f"= {100*non_image/tot:.2f}%")
    print("  → 这个比例就是电路仿真实际占用的 CPU 时间")
    print()
    print("=== 仿真代码（非 IMAGE 区）内的热点地址 Top 15 ===")
    sim = [(r, n) for r, n in hits.items()
           if region_of.get(r, (0, "IMAGE"))[1] != "IMAGE"]
    sim.sort(key=lambda x: -x[1])
    for rip, n in sim[:15]:
        base, ty = region_of[rip]
        print(f"  0x{rip:012X}  x{n:<5} (区 0x{base:012X} {ty}, 偏移 +0x{rip-base:X})")
    if not sim:
        print("  （无：仿真代码此刻没在跑）")


if __name__ == "__main__":
    sys.exit(main())

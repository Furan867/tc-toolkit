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

HERE_PID_HINT = "Turing Complete.exe"

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

TH32CS_SNAPTHREAD = 0x4
THREAD_GET_CONTEXT = 0x0008
THREAD_QUERY_INFORMATION = 0x0040
THREAD_SUSPEND_RESUME = 0x0002
CONTEXT_CONTROL = 0x00100001
CONTEXT_SIZE = 1232          # x64 CONTEXT 结构大小
RIP_OFFSET = 0xF8            # 其中 Rip 字段的偏移


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


def is_exec_non_image(h, addr):
    """地址是否落在「非 IMAGE 且可执行」的区（= JIT 代码）。"""
    mbi = MEMORY_BASIC_INFORMATION()
    if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi),
                              ctypes.sizeof(mbi)):
        return False
    return mbi.Type != 0x1000000 and bool(mbi.Protect & (0x10 | 0x20 | 0x40 | 0x80))


def find_sim_threads(h, hts, rounds=40, interval=0.0005):
    """粗采几轮，找出 RIP 一直在 JIT 区的线程 —— 那就是仿真线程。

    其余线程都泡在 ntdll 等待。锁定它之后采样效率提升约 70 倍：
    不必再为每个等待中的线程付一次挂起/恢复开销。

    判据用「过半命中」而非「命中若干次」，避免偶尔跑进 JIT 的线程被误认。
    """
    cand = Counter()
    order = list(hts.items())
    for _ in range(rounds):
        for tid, ht in order:
            if k32.SuspendThread(ht) == 0xFFFFFFFF:
                continue
            rip = read_rip(ht)
            k32.ResumeThread(ht)
            if rip and is_exec_non_image(h, rip):
                cand[tid] += 1
        time.sleep(interval)
    return {t for t, n in cand.items() if n >= max(1, rounds // 2)}


def sample(pid, iterations, interval, auto=True, only_tids=None, duration=None):
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
    elif auto:
        sim = find_sim_threads(h, hts)
        if sim:
            target = {t: ht for t, ht in hts.items() if t in sim}
            print(f"  自动锁定仿真线程 {sorted(sim)}（共 {len(hts)} 个线程）")
        else:
            print("  未探测到在 JIT 区运行的线程，改为采样全部线程")

    hits = Counter()
    per_thread = {}
    misses = 0
    # 按轮数或按时长采样。实测每次 Suspend/Resume 约 0.45ms，比 interval 大得多，
    # 所以「按时长」才是可控的口径 —— 按轮数换算不出稳定的耗时。
    deadline = time.time() + duration if duration else None
    n = 0
    while True:
        if deadline is not None:
            if time.time() >= deadline:
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
            else:
                misses += 1
        time.sleep(interval)

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
    ap.add_argument("--all-threads", action="store_true",
                    help="采样全部线程（默认自动锁定在 JIT 区运行的仿真线程）")
    ap.add_argument("--tid", type=int, action="append",
                    help="只采样指定线程 ID（可重复；给了就不自动探测）")
    args = ap.parse_args()

    pid = args.pid or find_pid()
    if not pid:
        print("未找到游戏进程", file=sys.stderr)
        return 1
    total = args.iterations * 5
    print(f"PID {pid}，约 {args.iterations} 轮采样"
          f"（每轮 {args.interval*1000:.0f}ms）...")
    hits, agg, nthreads, misses, per_thread, region_of = sample(
        pid, args.iterations, args.interval,
        auto=not args.all_threads, only_tids=set(args.tid) if args.tid else None,
        duration=args.duration or None)
    print(f"可采样线程 {nthreads} 个，读到 RIP {sum(hits.values())} 次，"
          f"失败 {misses} 次")
    if args.hist:
        with open(args.hist, "w", encoding="utf-8", newline="\n") as f:
            f.write("rip\tcount\n")
            for rip, n in sorted(hits.items()):
                f.write(f"{rip}\t{n}\n")
        print(f"完整直方图（{len(hits)} 个不同地址）→ {args.hist}")
    print()
    print("=== 各线程的活动分布（只列有非 ntdll/win32u 活动的线程）===")
    for tid, cnt in sorted(per_thread.items(),
                           key=lambda kv: -sum(kv[1].values())):
        total_t = sum(cnt.values())
        # 该线程最多的几个地址
        top = cnt.most_common(3)
        summary = "  ".join(f"0x{r:X}x{n}" for r, n in top)
        print(f"  TID {tid:<7} 采样 {total_t:>5}  |  {summary}")
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

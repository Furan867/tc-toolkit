#!/usr/bin/env python3
"""扫描 Turing Complete 进程内存，定位 JIT 生成的仿真代码。

原理
----
1) JIT 机器码：JIT 必须申请**可执行**内存。exe/dll 的代码段是 MEM_IMAGE
   （磁盘映射），而 JIT 代码是进程自己申请的 MEM_PRIVATE + PAGE_EXECUTE_*。
   这个组合在正常程序里几乎只可能是运行时生成的代码。

2) 生成的源码文本：exe 的 .rdata 里存着代码模板，实例化后的文本在**堆**上
   （Nim string）。所以只在 MEM_PRIVATE 的非执行区里搜特征串。

只读，不修改目标进程任何内存。
"""
from __future__ import annotations

import argparse
import ctypes
import glob
import os
import re
import sys
from concurrent.futures import ThreadPoolExecutor
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORK = os.path.join(ROOT, "work")

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010

MEM_COMMIT = 0x1000
MEM_PRIVATE = 0x20000
MEM_MAPPED = 0x40000
MEM_IMAGE = 0x1000000

TYPE_NAME = {MEM_PRIVATE: "PRIVATE", MEM_MAPPED: "MAPPED", MEM_IMAGE: "IMAGE"}

EXEC_MASK = 0x10 | 0x20 | 0x40 | 0x80  # PAGE_EXECUTE*


class MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_ulonglong),
        ("AllocationBase", ctypes.c_ulonglong),
        ("AllocationProtect", wintypes.DWORD),
        ("PartitionId", wintypes.WORD),
        ("RegionSize", ctypes.c_ulonglong),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
    ]


k32.VirtualQueryEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                               ctypes.POINTER(MBI), ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                                  ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.ReadProcessMemory.restype = wintypes.BOOL

# 生成代码的特征串。
# 只保留**实测会命中**的：早期版本还列了 Verilog 导出模板与旧语法串，
# 它们在当前版本的仿真代码里一次都没出现，留着等于白扫 6 遍。
# 需要扩展时直接在下面追加，或临时改本列表。
MARKERS = [
    b"get_setting(sim_short_circuit_component_id_1)",
    b"def mode_run(target_cycle: Int) None",
]


def find_pid(name="Turing Complete.exe"):
    """用 toolhelp 快照找进程 PID。"""
    TH32CS_SNAPPROCESS = 0x2

    class PROCESSENTRY32(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD),
                    ("cntUsage", wintypes.DWORD),
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
    if snap == wintypes.HANDLE(-1).value:
        return None
    try:
        e = PROCESSENTRY32()
        e.dwSize = ctypes.sizeof(PROCESSENTRY32)
        ok = k32.Process32First(snap, ctypes.byref(e))
        while ok:
            if e.szExeFile.decode("mbcs", "replace").lower() == name.lower():
                return e.th32ProcessID
            ok = k32.Process32Next(snap, ctypes.byref(e))
    finally:
        k32.CloseHandle(snap)
    return None


def iter_regions(h):
    addr = 0
    max_addr = 0x7FFFFFFFFFFF
    while addr < max_addr:
        mbi = MBI()
        got = k32.VirtualQueryEx(h, ctypes.c_void_p(addr),
                                 ctypes.byref(mbi), ctypes.sizeof(mbi))
        if not got:
            break
        yield mbi
        nxt = mbi.BaseAddress + mbi.RegionSize
        if nxt <= addr:
            break
        addr = nxt


def read_mem(h, base, size):
    buf = ctypes.create_string_buffer(size)
    n = ctypes.c_size_t(0)
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(base), buf, size,
                               ctypes.byref(n))
    if not ok:
        return None
    return buf.raw[:n.value]


def scan_markers(h, mbi, chunk=4 << 20):
    """在单个区内搜特征串，返回 [(地址, 特征串, 上下文片段)]"""
    hits = []
    size = mbi.RegionSize
    off = 0
    carry = b""
    # 按区的实际大小分配并复用。固定按 chunk 分配会让小区白白申请 4MB
    # —— 800 多个区累计就是数 GB 的无谓分配，反而比不优化更慢。
    buf = ctypes.create_string_buffer(min(chunk, size))
    nread = ctypes.c_size_t(0)
    while off < size:
        n = min(chunk, size - off)
        ok = k32.ReadProcessMemory(h, ctypes.c_void_p(mbi.BaseAddress + off),
                                   buf, n, ctypes.byref(nread))
        if not ok:
            off += n
            carry = b""
            continue
        data = buf.raw[:nread.value]
        blob = (carry + data) if carry else data
        base = mbi.BaseAddress + off - len(carry)
        for mk in MARKERS:
            start = 0
            while True:
                i = blob.find(mk, start)
                if i < 0:
                    break
                a = base + i
                ctx = blob[max(0, i - 60):i + 120]
                hits.append((a, mk, ctx))
                start = i + 1
                if len(hits) > 200:
                    return hits
        carry = blob[-512:]
        off += n
    return hits


TEXT_OK = set(range(0x20, 0x7F)) | {0x09, 0x0A, 0x0D}


def clean_text(raw):
    """丢弃二进制垃圾行，让输出能直接用文本编辑器打开。

    扩展窗口的边界会把少量非文本字节带进来（NimStringDesc 头部、
    相邻对象等），逐行按可打印比例过滤掉。
    """
    keep = []
    for line in raw.split(b"\n"):
        if not line:
            keep.append(b"")
            continue
        ok = sum(1 for b in line if b in TEXT_OK)
        if ok < len(line) * 0.9:
            continue                      # 整行是二进制，丢弃
        # 行内零星的非文本字节替换成空格，保证输出 100% 可读
        keep.append(bytes(b if b in TEXT_OK else 0x20 for b in line))
    return b"\n".join(keep)


def extract_text(h, addr, mbi, back=8 << 20, fwd=8 << 20):
    """在 mbi 所指内存区内，从 addr 向两侧扩展提取连续文本。

    必须限制在区内：跨区读取一旦碰到不可读页就整体失败，
    之前会静默返回 None，把命中的源码整份丢掉。
    """
    lo = mbi.BaseAddress
    hi = mbi.BaseAddress + mbi.RegionSize
    start = max(lo, addr - back)
    end = min(hi, addr + fwd)
    data = read_mem(h, start, end - start)
    if data is None:
        return None, None
    pos = addr - start
    gap = 0
    i = pos
    while i > 0:
        gap = 0 if data[i - 1] in TEXT_OK else gap + 1
        if gap > 8:
            break
        i -= 1
    gap = 0
    j = pos
    while j < len(data):
        gap = 0 if data[j] in TEXT_OK else gap + 1
        if gap > 8:
            break
        j += 1
    return clean_text(data[i:j]), start + i


def main():
    ap = argparse.ArgumentParser(description="扫描游戏进程定位生成的仿真代码")
    ap.add_argument("--pid", type=int, help="进程 PID（默认自动查找）")
    ap.add_argument("--dump-exec", action="store_true",
                    help="把 JIT 可执行区 dump 到 work/jit_dump_<base>.bin")
    ap.add_argument("--max-dump-mb", type=int, default=64,
                    help="单个可执行区最大 dump 体积(MB)")
    ap.add_argument("--threads", type=int, default=8,
                    help="并行扫描线程数。实测 4~20 差异很小（约 1.25s vs "
                         "1.30s），8 最优；再往上会被系统调用竞争吃掉收益")
    args = ap.parse_args()

    pid = args.pid or find_pid()
    if not pid:
        print("未找到 Turing Complete.exe 进程", file=sys.stderr)
        return 1
    print(f"进程 PID: {pid}")

    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        print(f"OpenProcess 失败 (错误 {ctypes.get_last_error()})。"
              f"若游戏以管理员运行，本脚本也需提权。", file=sys.stderr)
        return 1

    try:
        regions = list(iter_regions(h))
        print(f"内存区总数: {len(regions)}")

        # JIT 代码的实际落点是「非 IMAGE 且可执行」的区 —— 实测在 MEM_MAPPED
        # 的可写代码页里，只认 MEM_PRIVATE 会把它整个漏掉。
        priv_exec = []
        priv_data = []
        for m in regions:
            if m.State != MEM_COMMIT or m.Type == MEM_IMAGE:
                continue
            if m.Protect & EXEC_MASK:
                priv_exec.append(m)
            elif m.Type == MEM_PRIVATE:
                priv_data.append(m)

        print(f"  非 IMAGE 的可执行区 (JIT 代码候选): {len(priv_exec)} 个, "
              f"共 {sum(m.RegionSize for m in priv_exec) / 1048576:.1f} MB")
        print(f"  私有可写区 (堆/数据)             : {len(priv_data)} 个, "
              f"共 {sum(m.RegionSize for m in priv_data) / 1048576:.1f} MB")
        print()

        if priv_exec:
            print("=== JIT 代码候选区 ===")
            for m in sorted(priv_exec, key=lambda x: -x.RegionSize)[:25]:
                print(f"  0x{m.BaseAddress:012X}  "
                      f"{m.RegionSize / 1024:>9.1f} KB  "
                      f"prot=0x{m.Protect:02X}  "
                      f"{TYPE_NAME.get(m.Type, hex(m.Type))}")
            print()

        if args.dump_exec:
            os.makedirs(WORK, exist_ok=True)
            for m in sorted(priv_exec, key=lambda x: -x.RegionSize)[:10]:
                if m.RegionSize > args.max_dump_mb * 1048576:
                    print(f"  跳过 0x{m.BaseAddress:X}（超出 --max-dump-mb）")
                    continue
                data = read_mem(h, m.BaseAddress, m.RegionSize)
                if data is None:
                    print(f"  读取失败 0x{m.BaseAddress:X}")
                    continue
                out = os.path.join(WORK, f"jit_dump_{m.BaseAddress:012X}.bin")
                with open(out, "wb") as f:
                    f.write(data)
                print(f"  已写出 {out}  ({len(data)} 字节)")
            print()

        print("=== 在堆区搜索生成代码的特征串 ===")
        targets = [m for m in priv_data if m.RegionSize >= 4096]
        hits = []
        with ThreadPoolExecutor(max_workers=args.threads) as ex:
            for m, res in zip(targets,
                              ex.map(lambda mm: scan_markers(h, mm), targets)):
                hits.extend((a, m) for a, _, _ in res)
        print(f"命中 {len(hits)} 处"
              f"（{len(targets)} 个内存区，{args.threads} 线程）")

        if hits:
            os.makedirs(WORK, exist_ok=True)
            # 清掉上一轮产物：这次若少提取到几份，旧文件残留会误导后续分析
            for old in glob.glob(os.path.join(WORK, "generated_code_*.txt")):
                os.remove(old)
            seen = {}
            for a, m in hits[:12]:
                txt, base = extract_text(h, a, m)
                if not txt:
                    continue
                key = hash(txt)
                if key in seen:
                    continue
                seen[key] = (base, txt)

            print(f"去重后得到 {len(seen)} 份不同文本：")
            for idx, (base, txt) in enumerate(seen.values()):
                lines = txt.count(b"\n")
                print(f"  [文本{idx}] 起始 0x{base:012X}  "
                      f"{len(txt):,} 字节 / {lines:,} 行")
                out = os.path.join(WORK, f"generated_code_{idx}.txt")
                with open(out, "wb") as f:
                    f.write(txt)
                print(f"            写出 {out}")
        else:
            print("（未命中：生成的源码可能已被释放，或不是以纯文本形式常驻）")
    finally:
        k32.CloseHandle(h)
    return 0


if __name__ == "__main__":
    sys.exit(main())

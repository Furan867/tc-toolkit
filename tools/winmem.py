#!python3
"""跨脚本共用的 Windows 进程 / 内存访问工具。

这些函数原本散在 `catch_compile_source.py` 里，但被多个脚本 import。那个脚本
已经因为「调试器 + 硬件断点」方案崩了两次游戏而废弃，公共部分因此提取到这里 ——
废弃脚本自己反过来依赖本模块（废弃的是方案，不是这些只读的底层函数）。

**约定：本模块的所有函数只读目标进程** —— 不写内存、不改线程上下文、不附加调试器。
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import subprocess
import sys

# Windows 下 stdout 走管道时默认按 locale(GBK) 编码，输出里的 ✅/❌ 会抛
# UnicodeEncodeError 把流程打断。统一在这里处理一次。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

PROCESS_QUERY_INFO = 0x0400
PROCESS_VM_READ = 0x0010
THREAD_ALL_ACCESS = 0x001F0FFF

MEM_COMMIT = 0x1000
MEM_IMAGE = 0x1000000
PAGE_EXEC_MASK = 0x10 | 0x20 | 0x40 | 0x80     # EXECUTE / _READ / _READWRITE / _WRITECOPY
PAGE_GUARD = 0x100


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD),
                ("th32ProcessID", wt.DWORD), ("GlblcntUsage", wt.DWORD),
                ("ProccntUsage", wt.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", wt.DWORD), ("hModule", ctypes.c_void_p),
                ("szModule", ctypes.c_char * 256), ("szExePath", ctypes.c_char * 260)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                ("th32ThreadID", wt.DWORD), ("th32OwnerProcessID", wt.DWORD),
                ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
                ("dwFlags", wt.DWORD)]


class MBI(ctypes.Structure):
    """MEMORY_BASIC_INFORMATION (x64)。

    ⚠️ 字段宽度必须逐个对齐：BaseAddress / AllocationBase / RegionSize 是 8 字节，
    AllocationProtect / State / Protect / Type 是 4 字节，PartitionId 是 2 字节
    （后面还有 2 字节填充，ctypes 自己会补）。宽度写错的话整个结构的偏移全歪。
    """
    _fields_ = [("BaseAddress", ctypes.c_ulonglong),
                ("AllocationBase", ctypes.c_ulonglong),
                ("AllocationProtect", ctypes.c_uint32),
                ("PartitionId", ctypes.c_uint16),
                ("RegionSize", ctypes.c_ulonglong),
                ("State", ctypes.c_uint32),
                ("Protect", ctypes.c_uint32),
                ("Type", ctypes.c_uint32)]


k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.ReadProcessMemory.restype = wt.BOOL
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p,
                               ctypes.POINTER(MBI), ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t


def find_pid(name="Turing Complete.exe"):
    """按可执行文件名查 PID（不硬编码）。

    ⚠️ 用 subprocess 直接传参数列表，**不要**写成 shell 命令 —— 在 Git Bash 里
    `tasklist /FI ...` 的 `/FI` 会被 MSYS 当成路径转换成 `D:/opt/Git/FI`，命令直接失败。
    """
    try:
        out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}",
                              "/FO", "CSV", "/NH"],
                             capture_output=True, text=True).stdout.strip()
    except OSError:
        return None
    if not out.startswith('"'):
        return None
    try:
        return int(out.split('","')[1])
    except (IndexError, ValueError):
        return None


def find_module(pid, module_name):
    """枚举进程模块，返回 (基址, 完整路径)；没找到返回 (None, None)。"""
    TH32CS_SNAPMODULE, TH32CS_SNAPMODULE32 = 0x8, 0x10
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap == wt.HANDLE(-1).value:
        return None, None
    e = MODULEENTRY32()
    e.dwSize = ctypes.sizeof(MODULEENTRY32)
    try:
        if not k32.Module32First(snap, ctypes.byref(e)):
            return None, None
        while True:
            if e.szModule.decode("mbcs", "replace").lower() == module_name.lower():
                return e.modBaseAddr, e.szExePath.decode("mbcs", "replace")
            if not k32.Module32Next(snap, ctypes.byref(e)):
                return None, None
    finally:
        k32.CloseHandle(snap)


def list_threads(pid):
    TH32CS_SNAPTHREAD = 0x4
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    if snap == wt.HANDLE(-1).value:
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


def rpm(h, addr, size):
    """ReadProcessMemory；失败返回 None。"""
    if size <= 0:
        return b""
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        return None
    return buf.raw[:got.value]


def open_process(pid):
    return k32.OpenProcess(PROCESS_QUERY_INFO | PROCESS_VM_READ, False, pid)


def iter_exec_regions(h, skip_image=False, max_region=128 * 1024 * 1024):
    """枚举可执行内存区，产出 (基址, 区类型, 内容)。

    JIT 代码住在 MEM_MAPPED + RWX 的区里；IMAGE 区（exe/dll）默认也扫。
    超过 max_region 的区不整体读（太慢，且实测 JIT 区都是几百 KB）。
    """
    mbi = MBI()
    size = ctypes.sizeof(mbi)
    addr = 0
    limit = 0x7FFFFFFFFFFF
    while addr < limit:
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), size):
            break
        base, rsize = mbi.BaseAddress, mbi.RegionSize
        if rsize == 0:
            break
        if (mbi.State == MEM_COMMIT
                and (mbi.Protect & PAGE_EXEC_MASK)
                and not (mbi.Protect & PAGE_GUARD)
                and rsize <= max_region
                and not (skip_image and mbi.Type == MEM_IMAGE)):
            data = rpm(h, base, rsize)
            if data:
                yield base, mbi.Type, data
        addr = base + rsize

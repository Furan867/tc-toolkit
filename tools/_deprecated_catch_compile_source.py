#!python3
"""在游戏进程里 hook compile.dll 的 compile()，捕获它收到的电路源码。

为什么这样做
------------
之前抓源码是在堆里「搜」：扫特征串 → 向两侧扩展 → 遇非文本字节停。这条路
注定要面对两个无解的问题：源码**分段存放**（块间夹几 KB 非文本数据）、
**多份副本紧邻**（提取时吞进下一份，127 个 nand 读成 254 个）。

改成「接」。compile.dll 的 compile() 契约已被反汇编独立验证 ——

    Result* compile(Result* out /*rcx*/, NimStr* src /*rdx*/, int flags /*r8d*/)
    src = {len: int64, ptr}      字符数据在 ptr + 8

在 compile 入口下硬件执行断点（DR0），触发时该线程被内核冻结，此时读 rdx
拿到 NimStr 指针，再读 {len, ptr} 与 ptr+8 处的 len 字节 —— 就是**完整、精确、
唯一**的那份源码。Nim string 自带长度字段，不需要猜边界；而且它天然是「此刻
正在编译的电路」，**识别问题（哪个 code 文件是当前电路）随之消失**。

地址怎么来
----------
    compile 运行时地址 = compile.dll 基址 + compile 导出 RVA

- 基址：运行时用 Toolhelp32 枚举模块查（ASLR + 游戏重启都会变，**绝不能硬编码**）
- RVA：从 compile.dll 文件头解析导出表（硬编码会在 Steam 更新后失效）

用法
----
    python tools/catch_compile_source.py                # 抓一次就分离
    python tools/catch_compile_source.py --all          # 每次调用都抓
    python tools/catch_compile_source.py --timeout 300  # --all 模式下的上限

运行后**到游戏里切换电路**（切换会触发重新编译）。

⚠️ 会给游戏附加调试器（期间它被暂停，脚本用 ContinueDebugEvent 立刻放行）。
   脚本用 try/finally 保证分离，**并且分离前会清空所有线程的 DR0–DR3/DR7** ——
   硬件断点是 CPU 级状态，不清理的话调试器一走，游戏会因无人接管的 #DB 异常崩溃。
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import struct
import subprocess
import sys
import time

import tc_common

# Windows 下 stdout 被重定向或走管道时，Python 默认按 locale(GBK) 编码，脚本输出里
# 的 ✅/❌/★ 会直接抛 UnicodeEncodeError，把整个流程打断（实测踩到）。显式改 UTF-8，
# 无法编码的字符降级为替代符 —— 宁可显示成 ?，也不要中断。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORK = os.path.join(ROOT, "work")
GAME = str(tc_common.game_dir())
PID_FILE = os.path.join(WORK, "last_pid.txt")

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

# ── x64 CONTEXT 字段偏移（与 sample_threads.py / hw_breakpoint_trace.py 一致）──
CONTEXT_SIZE = 1232
CTX_FLAGS = 0x30
CTX_DR0, CTX_DR7 = 0x48, 0x70
CTX_RCX, CTX_RDX, CTX_RSP, CTX_RIP = 0x80, 0x88, 0x98, 0xF8

CONTEXT_DEBUG_REGISTERS = 0x00100010
CONTEXT_CONTROL = 0x00100001
CONTEXT_INTEGER = 0x00000002
CTX_ALL = CONTEXT_DEBUG_REGISTERS | CONTEXT_CONTROL | CONTEXT_INTEGER

# DR7：L0=1 启用 DR0；RW0=00 → 执行断点；LEN0=00 → 1 字节。
# bit 10 是 Intel 建议置 1 的保留位（旧脚本没置，无从判断是否有影响）。
DR7_EXEC = (1 << 10) | 1

# 调试事件码
EXCEPTION_DEBUG_EVENT = 1
CREATE_THREAD_DEBUG_EVENT = 2
CREATE_PROCESS_DEBUG_EVENT = 3
EXIT_THREAD_DEBUG_EVENT = 4
EXIT_PROCESS_DEBUG_EVENT = 5
LOAD_DLL_DEBUG_EVENT = 6
UNLOAD_DLL_DEBUG_EVENT = 7
OUTPUT_DEBUG_STRING_EVENT = 8
RIP_EVENT = 9

EXCEPTION_SINGLE_STEP = 0x80000004
EXCEPTION_BREAKPOINT = 0x80000003
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001

THREAD_ALL_ACCESS = 0x001F0FFF
PROCESS_QUERY_INFO = 0x0400
PROCESS_VM_READ = 0x0010


class DEBUG_EVENT(ctypes.Structure):
    _fields_ = [("dwDebugEventCode", wt.DWORD), ("dwProcessId", wt.DWORD),
                ("dwThreadId", wt.DWORD), ("u", ctypes.c_byte * 176)]


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


k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenThread.restype = wt.HANDLE
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.GetThreadContext.restype = wt.BOOL
k32.SetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.SetThreadContext.restype = wt.BOOL
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.ReadProcessMemory.restype = wt.BOOL
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE


# ────────────────────────────── 定位 ──────────────────────────────

def find_pid(name=tc_common.GAME_EXE_NAME):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout.strip()
    if not out.startswith('"'):
        return None
    try:
        return int(out.split('","')[1])
    except (IndexError, ValueError):
        return None


def find_module(pid, module_name):
    """枚举进程模块，返回 (基址, 完整路径)；没找到返回 (None, None)。"""
    TH32CS_SNAPMODULE = 0x8
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | 0x10, pid)
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


def export_rva(dll_path, func_name):
    """从 PE 文件解析导出表，返回该导出的 RVA；失败返回 None。

    自己解析而不是调用工具，是为了让脚本在 Steam 更新后仍能自动跟上 ——
    硬编码的 RVA 一定会过期，而过期的地址会让断点静默失效（踩过）。
    """
    try:
        data = open(dll_path, "rb").read()
    except OSError as exc:
        print(f"  读不了 {dll_path}: {exc}", file=sys.stderr)
        return None
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        return None
    coff = e_lfanew + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    mag = struct.unpack_from("<H", data, opt)[0]
    dd = opt + (112 if mag == 0x20B else 96)       # 数据目录起点
    exp_rva = struct.unpack_from("<I", data, dd)[0]
    sec = opt + opt_size

    def rva2off(rva):
        for i in range(nsec):
            s = sec + i * 40
            va = struct.unpack_from("<I", data, s + 12)[0]
            vs = struct.unpack_from("<I", data, s + 8)[0]
            raw = struct.unpack_from("<I", data, s + 20)[0]
            if va <= rva < va + max(vs, 1):
                return raw + (rva - va)
        return None

    eo = rva2off(exp_rva)
    if eo is None:
        return None
    nnames = struct.unpack_from("<I", data, eo + 24)[0]
    fn_tbl = rva2off(struct.unpack_from("<I", data, eo + 28)[0])
    nm_tbl = rva2off(struct.unpack_from("<I", data, eo + 32)[0])
    or_tbl = rva2off(struct.unpack_from("<I", data, eo + 36)[0])
    for i in range(nnames):
        no = rva2off(struct.unpack_from("<I", data, nm_tbl + i * 4)[0])
        if no is None:
            continue
        end = data.index(b"\0", no)
        if data[no:end].decode("ascii", "replace") == func_name:
            ordi = struct.unpack_from("<H", data, or_tbl + i * 2)[0]
            return struct.unpack_from("<I", data, fn_tbl + ordi * 4)[0]
    return None


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


# ────────────────────── 调试寄存器：设点 / 清点 ──────────────────────

def _with_ctx(tid, mutate):
    ht = k32.OpenThread(THREAD_ALL_ACCESS, False, tid)
    if not ht:
        return False
    try:
        ctx = ctypes.create_string_buffer(CONTEXT_SIZE)
        ctypes.memset(ctx, 0, CONTEXT_SIZE)
        struct.pack_into("<I", ctx, CTX_FLAGS, CTX_ALL)
        if not k32.GetThreadContext(ht, ctx):
            return False
        mutate(ctx)
        return bool(k32.SetThreadContext(ht, ctx))
    finally:
        k32.CloseHandle(ht)


def arm(tid, addr):
    """给线程设 DR0 = addr 的执行断点。"""
    def mutate(ctx):
        struct.pack_into("<Q", ctx, CTX_DR0, addr)
        struct.pack_into("<Q", ctx, CTX_DR7, DR7_EXEC)
    return _with_ctx(tid, mutate)


def disarm_all(pid, rounds=8):
    """循环清空所有线程的调试寄存器，直到**验证**干净。

    为什么不能只清一遍：设点时枚举到 71 个线程，分离时可能只枚举到 70 个
    （线程在期间创建/销毁），漏掉的那个就是定时炸弹 —— 调试器一走，它一旦
    执行到断点地址就抛 #DB，无人接管，进程直接崩（实测踩到，0xC0000005）。

    返回 (枚举到的线程数, 残留非零 DR0 的线程数, 读不到上下文的线程数)。
    """
    total = left = unreadable = 0
    for _ in range(rounds):
        tids = list_threads(pid)
        total = len(tids)
        dirty, unreadable = [], 0
        for t in tids:
            v = dr0_of(t)
            if v is None:
                unreadable += 1
            elif v:
                dirty.append(t)
        if not dirty:
            return total, 0, unreadable
        for t in dirty:
            disarm(t)
    # 收尾复核
    tids = list_threads(pid)
    left = unreadable = 0
    for t in tids:
        v = dr0_of(t)
        if v is None:
            unreadable += 1
        elif v:
            left += 1
    return len(tids), left, unreadable


def debug_regs_of(tid):
    """读回该线程的 DR0 与 DR7。

    ⚠️ 必须**同时**看 DR7：DR0 里有地址 ≠ 断点已启用，启用位是 DR7 的 bit0(L0)。
    只看 DR0 会得出「设点成功了」的错误结论，而断点其实永远不触发 ——
    这是最难查的一类失败。返回 (dr0, dr7)，读不到则 (None, None)。
    """
    ht = k32.OpenThread(THREAD_ALL_ACCESS, False, tid)
    if not ht:
        return None, None
    try:
        ctx = ctypes.create_string_buffer(CONTEXT_SIZE)
        ctypes.memset(ctx, 0, CONTEXT_SIZE)
        struct.pack_into("<I", ctx, CTX_FLAGS, CTX_ALL)
        if not k32.GetThreadContext(ht, ctx):
            return None, None
        return (struct.unpack_from("<Q", ctx, CTX_DR0)[0],
                struct.unpack_from("<Q", ctx, CTX_DR7)[0])
    finally:
        k32.CloseHandle(ht)


def dr0_of(tid):
    return debug_regs_of(tid)[0]


def disarm(tid):
    """清空该线程的 DR0–DR3 与 DR7。

    **必须做**：DR 是 CPU 级状态，调试器分离后若仍置位，执行到那里会抛 #DB，
    而已经没有调试器接管它 —— 游戏直接崩。旧脚本没做这一步。
    """
    def mutate(ctx):
        for off in (CTX_DR0, CTX_DR0 + 8, CTX_DR0 + 16, CTX_DR0 + 24, CTX_DR7):
            struct.pack_into("<Q", ctx, off, 0)
    return _with_ctx(tid, mutate)


def read_regs(pid, tid):
    """读触发线程的 RCX/RDX/RSP/RIP（线程此刻已被内核冻结，可直接读）。"""
    ht = k32.OpenThread(THREAD_ALL_ACCESS, False, tid)
    if not ht:
        return None
    try:
        ctx = ctypes.create_string_buffer(CONTEXT_SIZE)
        ctypes.memset(ctx, 0, CONTEXT_SIZE)
        struct.pack_into("<I", ctx, CTX_FLAGS, CTX_ALL)
        if not k32.GetThreadContext(ht, ctx):
            return None
        g = lambda off: struct.unpack_from("<Q", ctx, off)[0]
        return {"rcx": g(CTX_RCX), "rdx": g(CTX_RDX),
                "rsp": g(CTX_RSP), "rip": g(CTX_RIP)}
    finally:
        k32.CloseHandle(ht)


def rpm(h, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        return None
    return buf.raw[:got.value]


def read_nim_string(h, nimstr_ptr, max_len=64 * 1024 * 1024):
    """读 {len, ptr} 并取出 ptr+8 处的 len 字节（compile() 的契约）。"""
    head = rpm(h, nimstr_ptr, 16)
    if not head or len(head) < 16:
        return None, "读不到 NimStr 头"
    n, ptr = struct.unpack("<qQ", head)
    if n <= 0:
        return None, f"长度非法 ({n})"
    if n > max_len:
        return None, f"长度过大 ({n}) —— 可能不是 NimStr"
    if not ptr:
        return None, "ptr 为空"
    body = rpm(h, ptr + 8, n)          # ⚠ 字符数据在 ptr + 8，不是 ptr
    if body is None:
        return None, "读不到字符数据"
    return body, None


# ────────────────────────────── 主流程 ──────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="hook compile() 捕获电路源码")
    ap.add_argument("--pid", type=int, help="进程 PID（默认自动查找）")
    ap.add_argument("--all", action="store_true", help="每次调用都抓（默认抓一次就分离）")
    ap.add_argument("--timeout", type=float, default=180,
                    help="最长等待秒数（超时即清理调试寄存器并分离）")
    ap.add_argument("--out", default=os.path.join(WORK, "caught_source.bin"),
                    help="源码写到哪")
    ap.add_argument("--probe-addr",
                    help="验证模式：在指定地址（十六进制）设执行断点，命中即报告，"
                         "不解析内存。用来区分「断点机制没生效」与「函数没被调用」")
    args = ap.parse_args()

    pid = args.pid or find_pid()
    if not pid:
        print("❌ 未找到 Turing Complete.exe —— 游戏没在运行？", file=sys.stderr)
        return 1

    # PID 变化检查（所有内存地址都依赖进程实例，重启即全部失效）
    os.makedirs(WORK, exist_ok=True)
    prev = None
    if os.path.exists(PID_FILE):
        try:
            prev = int(open(PID_FILE).read().strip())
        except ValueError:
            prev = None
    if prev and prev != pid:
        print(f"⚠️  PID 变了（上次 {prev} → 现在 {pid}）："
              f"上次会话留下的地址全部失效，本脚本会重新查。")
    open(PID_FILE, "w").write(str(pid))

    if args.probe_addr:
        # 验证模式：不查 dll，只确认「硬件执行断点到底会不会触发」。
        # 用来把「机制没生效」和「目标函数没被调用」这两件事彻底分开 ——
        # 否则失败时永远在猜是哪种。
        target = int(args.probe_addr, 16)
        print(f"【验证模式】目标 0x{target:X} —— 只确认断点会不会触发，不解析内存")
    else:
        base, path = find_module(pid, "compile.dll")
        if not base:
            print("❌ 游戏进程里没有 compile.dll —— 电路还没编译过？", file=sys.stderr)
            return 1
        if not path:
            path = os.path.join(GAME, "compile.dll")
        rva = export_rva(path, "compile")
        if rva is None:
            print(f"❌ 从 {path} 解析不出 compile 导出", file=sys.stderr)
            return 1
        target = base + rva
        print(f"PID {pid}   compile.dll 基址 0x{base:X}   compile RVA 0x{rva:X}")
        print(f"→ 断点目标地址 0x{target:X}")

    if not k32.DebugActiveProcess(pid):
        print(f"❌ DebugActiveProcess 失败，错误码 {ctypes.get_last_error()}",
              file=sys.stderr)
        return 1
    print("已附加为调试器。👉 现在到游戏里**切换电路**（切换会触发重新编译）...\n")

    h = k32.OpenProcess(PROCESS_QUERY_INFO | PROCESS_VM_READ, False, pid)
    if not h:
        print("❌ OpenProcess 失败（读不了目标内存）", file=sys.stderr)
        k32.DebugActiveProcessStop(pid)
        return 1

    caught, armed, t0 = 0, set(), time.time()
    try:
        while True:
            if time.time() - t0 > args.timeout:
                print(f"\n⏱ 等待 {args.timeout:.0f}s 未捕获到调用，结束。")
                break
            ev = DEBUG_EVENT()
            if not k32.WaitForDebugEvent(ctypes.byref(ev), 500):
                continue
            code, tid = ev.dwDebugEventCode, ev.dwThreadId
            action = DBG_CONTINUE

            if code in (CREATE_PROCESS_DEBUG_EVENT, CREATE_THREAD_DEBUG_EVENT):
                if arm(tid, target):
                    armed.add(tid)
                if code == CREATE_PROCESS_DEBUG_EVENT:
                    print(f"  已附加，为已存在的 {len(armed)} 个线程设点中 ...")

            elif code == EXCEPTION_DEBUG_EVENT:
                exc = struct.unpack_from("<I", ev.u, 0)[0]
                if exc == EXCEPTION_SINGLE_STEP:
                    regs = read_regs(pid, tid)
                    if regs:
                        print(f"★ 命中！TID {tid}  RIP 0x{regs['rip']:X}"
                              f"  rcx=0x{regs['rcx']:X}  rdx=0x{regs['rdx']:X}")
                        if args.probe_addr:
                            caught += 1
                            if not args.all:
                                print("  ✅ 硬件执行断点确实会触发 —— 机制没问题。")
                                break
                        else:
                            body, err = read_nim_string(h, regs["rdx"])
                            if body is None:
                                print(f"  ⚠ 取源码失败：{err}")
                            else:
                                caught += 1
                                out = args.out if not args.all else \
                                    args.out.replace(".bin", f"_{caught}.bin")
                                with open(out, "wb") as f:
                                    f.write(body)
                                head = body[:160].decode("utf-8", "replace")
                                print(f"  ✅ 拿到源码 {len(body)} 字节 → {out}")
                                print(f"     开头: {head.splitlines()[0] if head else ''}")
                                if not args.all:
                                    print("\n抓到一份，分离调试器。")
                                    break
                    else:
                        print(f"  ⚠ 命中但读不到寄存器（TID {tid}）")
                elif exc == EXCEPTION_BREAKPOINT:
                    action = DBG_CONTINUE          # 附加调试器时的固有断点
                else:
                    action = DBG_EXCEPTION_NOT_HANDLED

            elif code == EXIT_PROCESS_DEBUG_EVENT:
                print("进程退出。")
                break

            k32.ContinueDebugEvent(ev.dwProcessId, tid, action)
    finally:
        k32.CloseHandle(h)
        # 分离前自检 + 彻底清理，两件事必须一起做：
        #   ① 自检：断点是否真挂在线程上。若一个都没有，说明 SetThreadContext
        #      报成功却并未生效 —— 那是**机制**问题，不是「compile() 没被调用」。
        #      这两者不分开，就会一直误以为是「没触发 → 再跑一次试试」。
        #   ② 清理：DR 是 CPU 级状态。调试器一走，残留的 DR0 被执行到就抛 #DB，
        #      无人接管 → 进程崩。必须**循环清理 + 复验**，只清一遍会漏。
        tids = list_threads(pid)
        still, dr7s = 0, {}
        for t in tids:
            d0, d7 = debug_regs_of(t)
            if d0 == target:
                still += 1
                dr7s[d7] = dr7s.get(d7, 0) + 1
        print(f"\n自检：{len(tids)} 个线程里，{still} 个的 DR0 仍指向 0x{target:X}")
        if still == 0:
            print("   ⚠ 一个都没挂上 —— 断点没设上，不是「没调用」")
        for d7, n in sorted(dr7s.items(), key=lambda kv: -kv[1])[:3]:
            flag = "✅ 已启用" if (d7 & 1) else "❌ L0=0 → 断点从未启用！"
            print(f"   {n} 个线程的 DR7 = 0x{d7:X}   {flag}")
        total, left, unreadable = disarm_all(pid)
        k32.DebugActiveProcessStop(pid)
        print(f"已分离（运行期间设过点的线程 {len(armed)} 个，清理时枚举到 {total} 个）")
        if left or unreadable:
            print(f"   ❌ 清理不彻底：残留非零 DR0 的 {left} 个、读不到上下文的 "
                  f"{unreadable} 个 —— 游戏执行到 0x{target:X} 会崩！")
        else:
            print("   ✅ 全部线程的 DR0 已确认归零")

    if not caught:
        print("⚠ 未捕获到任何 compile() 调用 —— 切换电路了吗？")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!python3
"""硬件断点追踪：找出「谁在写某个内存地址」。

CE 里那个 "Find out what writes to this address" 的 Python 版 —— 用调试寄存器
DR0–DR3 设写断点，把游戏附加为调试对象，在断点触发时捕获 RIP。拿到 RIP 就能
反汇编那段代码，直接看是谁在更新这个字段（不靠猜结构）。

用法：
    python tools/hw_breakpoint_trace.py --addr 0x25A5CADCBB7 --timeout 60
    运行后到游戏里切换电路 —— 写入发生时断点触发，脚本打印 RIP 并自动分离。

⚠️ 会给游戏附加调试器（期间它被暂停，脚本用 ContinueDebugEvent 立刻放行）。
   万一游戏有反调试，可能崩掉 —— 重启即可，存档不受影响。
   脚本用 finally 保证分离，不会把游戏永远挂住。
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import struct
import subprocess
import sys
import time

import tc_common

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

# x64 CONTEXT：只用到这几个字段的偏移
CONTEXT_SIZE = 1232
FLAGS_OFF, DR0_OFF, DR7_OFF, RIP_OFF = 0x30, 0x48, 0x70, 0xF8
CONTEXT_DEBUG_REGISTERS = 0x00100010
CONTEXT_CONTROL = 0x00100001

CREATE_PROCESS_DEBUG_EVENT = 3
CREATE_THREAD_DEBUG_EVENT = 2
EXCEPTION_DEBUG_EVENT = 1
EXCEPTION_SINGLE_STEP = 0x80000004
DBG_CONTINUE = 0x00010002

# DR7：L0=1 启用 DR0，LEN0=10 监控 8 字节
#   RW0=01 只断「写」—— 用于「谁改了这个值」
#   RW0=11 读写都断 —— 断「读」能抓到「谁在读它」（UI 刷新、代码遍历）
DR7_WRITE8 = (0b10 << 18) | (0b01 << 16) | 1        # = 0x90001
DR7_RW8 = (0b10 << 18) | (0b11 << 16) | 1           # = 0xB0001


class DEBUG_EVENT(ctypes.Structure):
    _fields_ = [("dwDebugEventCode", wt.DWORD),
                ("dwProcessId", wt.DWORD),
                ("dwThreadId", wt.DWORD),
                ("u", ctypes.c_byte * 176)]          # 容纳各事件的联合体


def find_pid(name=tc_common.GAME_EXE_NAME):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout.strip()
    if not out.startswith('"'):
        return None
    try:
        return int(out.split('","')[1])
    except (IndexError, ValueError):
        return None


def set_watch(tid, addr):
    """给线程设 DR0 = addr、DR7 = 8 字节写断点。"""
    ht = k32.OpenThread(0x001F0FFF, False, tid)       # THREAD_ALL_ACCESS
    if not ht:
        return False
    ctx = ctypes.create_string_buffer(CONTEXT_SIZE)
    ctypes.memset(ctx, 0, CONTEXT_SIZE)
    struct.pack_into("<I", ctx, FLAGS_OFF, CONTEXT_DEBUG_REGISTERS | CONTEXT_CONTROL)
    ok = k32.GetThreadContext(ht, ctx)
    if ok:
        struct.pack_into("<Q", ctx, DR0_OFF, addr)
        struct.pack_into("<Q", ctx, DR7_OFF, DR7_RW8)
        ok = k32.SetThreadContext(ht, ctx)
    k32.CloseHandle(ht)
    return bool(ok)


def main():
    ap = argparse.ArgumentParser(description="硬件断点追踪：谁在写这个地址")
    ap.add_argument("--addr", required=True, help="目标地址（十六进制，如 0x25A5CADCBB7）")
    ap.add_argument("--pid", type=int, help="进程 PID（默认自动查找）")
    ap.add_argument("--timeout", type=float, default=60, help="最长等待秒数")
    args = ap.parse_args()

    addr = int(args.addr, 16)
    pid = args.pid or find_pid()
    if not pid:
        print("未找到 Turing Complete.exe", file=sys.stderr)
        return 1
    print(f"目标地址 0x{addr:X}   PID {pid}   超时 {args.timeout:.0f}s")

    if not k32.DebugActiveProcess(pid):
        print(f"DebugActiveProcess 失败，错误码 {ctypes.get_last_error()}", file=sys.stderr)
        return 1
    print("已附加为调试器。👉 现在到游戏里切换电路（切换会写这个地址）...")
    t0, hit, nset = time.time(), None, 0
    try:
        while time.time() - t0 < args.timeout:
            ev = DEBUG_EVENT()
            if not k32.WaitForDebugEvent(ctypes.byref(ev), 500):
                continue
            code = ev.dwDebugEventCode
            if code in (CREATE_PROCESS_DEBUG_EVENT, CREATE_THREAD_DEBUG_EVENT):
                if set_watch(ev.dwThreadId, addr):
                    nset += 1
            elif code == EXCEPTION_DEBUG_EVENT:
                exc_code = struct.unpack_from("<I", ev.u, 0)[0]
                exc_addr = struct.unpack_from("<Q", ev.u, 0x10)[0]
                if exc_code == EXCEPTION_SINGLE_STEP:
                    hit = exc_addr
                    print(f"\n★ 断点命中！写 0x{addr:X} 的指令位于 RIP = 0x{exc_addr:X}")
                    k32.ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE)
                    break
            k32.ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE)
    finally:
        k32.DebugActiveProcessStop(pid)
        print(f"已分离调试器（为 {nset} 个线程设过断点）。")
    if not hit:
        print("超时未命中 —— 这段时间里没有代码写过该地址。")
    return 0


if __name__ == "__main__":
    sys.exit(main())

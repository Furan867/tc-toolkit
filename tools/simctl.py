#!/usr/bin/env python3
"""仿真控制工具：run / stop / recompile。

设计见 docs/specs/2026-10-05-sim-ctl-design.md。

三个动作：
    run()        写内存启动仿真（commands[0]=0 run）
    stop()       写内存停止仿真（commands[0]=3 quit_simulation）
    recompile()  注入按键触发重编译（SendInput 发 Ctrl+Z / Ctrl+Shift+Z）

recompile 走 UI 注入：让游戏自己走「电路变化 → 重建场景 → 编译」那条路。
不开自动检测（不监听 opt.ini），不判断编译是否成功（编译必经 compile.dll，
调试看转发层日志或主频工具）。

命令行用法：
    python tools/simctl.py run
    python tools/simctl.py stop
    python tools/simctl.py recompile            # 发撤销
    python tools/simctl.py recompile --redo     # 发重做

CLI 每次都是新进程，记不住「上次发的哪个键」，所以由调用者指定；
脚本内循环请 import 本模块，recompile() 会自动交替并在进程退出时恢复撤销栈。
"""
from __future__ import annotations

import argparse
import atexit
import contextlib
import ctypes
import os
import signal
import struct
import sys
import time
from ctypes import wintypes
from dataclasses import dataclass

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import winmem  # noqa: E402

EXE = "Turing Complete.exe"
BASE_VA = 0x140000000
OFF_CMDS = 0x1406F1E08 - BASE_VA      # simulation_commands（.bss 指针，解引用一层）
OFF_SETS = 0x1406F1E00 - BASE_VA      # simulation_settings（同上）
OFF_NEXT_ID = 0x14049B248 - BASE_VA   # simulation_next_command_id

INT64_MAX = 0x7FFFFFFFFFFFFFFF
SIM_RUN = 0        # SimCommand.run
SIM_QUIT = 3       # SimCommand.quit_simulation（「停止」按钮与「编辑电路时自动停止」共用）

PROCESS_QUERY = 0x0400
PROCESS_VM_READ = 0x0010
PROCESS_VM_WRITE = 0x0020
PROCESS_VM_OPERATION = 0x0008

WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
INPUT_KEYBOARD, KEYEVENTF_KEYUP = 1, 0x0002
VK_CONTROL, VK_SHIFT, VK_Z, VK_MENU = 0x11, 0x10, 0x5A, 0x12
GW_OWNER = 4


@dataclass
class Result:
    ok: bool
    action: str
    detail: str = ""

    def __bool__(self) -> bool:
        return self.ok

    def __str__(self) -> str:
        head = "ok" if self.ok else "失败"
        tail = ("  " + self.detail) if self.detail else ""
        return f"{head}: {self.action}{tail}"


# ---------------------------------------------------------------- 进程与内存

def _target() -> tuple[int, int] | None:
    """(pid, base)。游戏没运行返回 None。"""
    pid = winmem.find_pid(EXE)
    if not pid:
        return None
    base, _size = winmem.find_module(pid, EXE)
    if not base:
        return None
    return pid, base


def _open_rw(pid: int) -> int:
    h = winmem.k32.OpenProcess(
        PROCESS_QUERY | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
        False, pid)
    if not h:
        raise OSError(f"OpenProcess 失败 (errno {ctypes.get_last_error()})")
    return h


def _rd64(h: int, addr: int) -> int | None:
    raw = winmem.rpm(h, addr, 8)
    return None if raw is None else struct.unpack_from("<Q", raw)[0]


def _wr64(h: int, addr: int, value: int) -> None:
    buf = ctypes.create_string_buffer(struct.pack("<Q", value), 8)
    got = ctypes.c_size_t(0)
    ok = winmem.k32.WriteProcessMemory(h, ctypes.c_void_p(addr), buf, 8,
                                       ctypes.byref(got))
    if not ok or got.value != 8:
        raise OSError(f"WriteProcessMemory 失败 @0x{addr:X} "
                      f"(errno {ctypes.get_last_error()})")


def _publish(h: int, base: int, cmd: int, *, set_target: bool = False) -> int:
    """按游戏的规矩发一条命令：写 command + 新 id + 推进它自己的计数器。

    返回用的新 id。游戏 JIT 循环以 commands[1] 为键，id 必须比它自己见过的都新。
    """
    cmds = _rd64(h, base + OFF_CMDS)
    sets = _rd64(h, base + OFF_SETS)
    if not cmds or not sets:
        raise OSError("simulation_commands/settings 指针为空（电路可能还没加载）")
    counter = _rd64(h, base + OFF_NEXT_ID) or 0
    cur_id = _rd64(h, cmds + 8) or 0
    new_id = max(counter, cur_id) + 1
    if set_target:
        _wr64(h, sets + 8, INT64_MAX)      # settings[1] sim_target_cycle
    _wr64(h, cmds + 0, cmd)                # commands[0] ctl_command
    _wr64(h, cmds + 8, new_id)             # commands[1] ctl_command_id
    _wr64(h, base + OFF_NEXT_ID, new_id + 1)
    return new_id


# ------------------------------------------------------------------- 两个动作

def run() -> Result:
    """启动仿真运行。"""
    t = _target()
    if not t:
        return Result(False, "run", "游戏未运行")
    pid, base = t
    try:
        h = _open_rw(pid)
        new_id = _publish(h, base, SIM_RUN, set_target=True)
    except OSError as e:
        return Result(False, "run", str(e))
    return Result(True, "run", f"command_id={new_id} target=INT64_MAX pid={pid}")


def stop() -> Result:
    """停止仿真（quit_simulation）。"""
    t = _target()
    if not t:
        return Result(False, "stop", "游戏未运行")
    pid, base = t
    try:
        h = _open_rw(pid)
        new_id = _publish(h, base, SIM_QUIT)
    except OSError as e:
        return Result(False, "stop", str(e))
    return Result(True, "stop", f"command_id={new_id} pid={pid}")


# ------------------------------------------------------- 窗口 / 抢前台 / 按键

_u32 = ctypes.WinDLL("user32", use_last_error=True)
_k32 = ctypes.WinDLL("kernel32", use_last_error=True)

_u32.EnumWindows.argtypes = [ctypes.c_void_p, wintypes.LPARAM]
_u32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
_u32.GetWindow.restype = wintypes.HWND
_u32.GetClassNameW.argtypes = [wintypes.HWND, ctypes.c_wchar_p, ctypes.c_int]
_u32.GetWindowThreadProcessId.argtypes = [wintypes.HWND,
                                          ctypes.POINTER(wintypes.DWORD)]
_u32.GetWindowThreadProcessId.restype = wintypes.DWORD
_u32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
_u32.GetWindowTextW.argtypes = [wintypes.HWND, ctypes.c_wchar_p, ctypes.c_int]
_u32.IsWindowVisible.argtypes = [wintypes.HWND]
_u32.GetForegroundWindow.restype = wintypes.HWND
_u32.SetForegroundWindow.argtypes = [wintypes.HWND]
_u32.BringWindowToTop.argtypes = [wintypes.HWND]
_u32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
_u32.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
_u32.SendInput.argtypes = [wintypes.UINT, ctypes.c_void_p, ctypes.c_int]
_k32.GetCurrentThreadId.restype = wintypes.DWORD


class _KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_void_p)]


class _INPUTunion(ctypes.Union):
    _fields_ = [("ki", _KEYBDINPUT), ("pad", ctypes.c_byte * 32)]


class _INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _INPUTunion)]


def game_windows(pid: int) -> list[tuple[int, str, bool, bool, int]]:
    """该进程的顶层窗口：(hwnd, 标题, 是否可见, 是否对话框, owner)。"""
    out: list[tuple[int, str, bool, bool, int]] = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        p = wintypes.DWORD()
        _u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid:
            n = _u32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(n + 1)
            _u32.GetWindowTextW(hwnd, buf, n + 1)
            # 错误对话框的类名是 #32770（Win32 dialog）；它有 owner，主窗口没有。
            cbuf = ctypes.create_unicode_buffer(256)
            _u32.GetClassNameW(hwnd, cbuf, 256)
            is_dialog = cbuf.value == "#32770"
            owner = _u32.GetWindow(hwnd, GW_OWNER) or 0
            out.append((hwnd, buf.value, bool(_u32.IsWindowVisible(hwnd)),
                        is_dialog, owner))
        return True

    _u32.EnumWindows(cb, 0)
    return out


def blocking_dialog(pid: int) -> tuple[int, str] | None:
    """检测游戏是否弹出了错误对话框（CRT assert 框等）。

    有对话框就意味着游戏已经处于错误状态，这时候绝不能碰它 ——
    上一次实测中，脚本把 CRT 错误框当成了主窗口，还给它发了按键。
    """
    for hwnd, title, visible, is_dialog, _owner in game_windows(pid):
        if visible and is_dialog:
            return hwnd, (title or "(无标题对话框)")
    return None


def find_game_window(pid: int) -> int | None:
    """游戏主窗口。

    必须排除对话框（#32770）和有 owner 的窗口，否则会把 CRT 错误框当成主窗口。
    """
    for hwnd, title, visible, is_dialog, owner in game_windows(pid):
        if visible and title and not is_dialog and not owner:
            return hwnd
    return None


def _window_owner(hwnd: int) -> str:
    if not hwnd:
        return "(无)"
    p = wintypes.DWORD()
    _u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
    n = _u32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(n + 1)
    _u32.GetWindowTextW(hwnd, buf, n + 1)
    title = buf.value[:50] or "(无标题)"
    return f"[pid {p.value}] {title}"


def _send_input(vk: int, up: bool) -> None:
    inp = _INPUT(type=INPUT_KEYBOARD)
    inp.u.ki = _KEYBDINPUT(wVk=vk, wScan=0, dwExtraInfo=None,
                           dwFlags=KEYEVENTF_KEYUP if up else 0, time=0)
    if _u32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(_INPUT)) != 1:
        raise OSError(f"SendInput 失败 (errno {ctypes.get_last_error()})")


def force_foreground(hwnd: int) -> bool:
    """从后台进程把窗口抢到前台。

    直接 SetForegroundWindow 会被前台锁拒绝（只让任务栏闪一下），所以：
      1. 先发一次 Alt —— 系统放宽前台限制
      2. AttachThreadInput 把自己的输入队列接到前台线程，再抢

    刻意不调 ShowWindow / BringWindowToTop —— 它们会改变窗口的显示状态
    （SW_RESTORE 会让 ImGui 重算视口与 font scale），实测疑似触发了
    game_engine.dll 里的 ImGui 断言 `scale > 0.0f`。抢前台只做「把已有窗口
    提到最前」，不动它的尺寸与可见性。
    """
    if _u32.GetForegroundWindow() == hwnd:
        return True
    _send_input(VK_MENU, False)
    time.sleep(0.05)
    _send_input(VK_MENU, True)
    time.sleep(0.05)

    fg = _u32.GetForegroundWindow()
    fg_thread = _u32.GetWindowThreadProcessId(fg, None) if fg else 0
    cur = _k32.GetCurrentThreadId()
    attached = False
    if fg_thread and fg_thread != cur:
        attached = bool(_u32.AttachThreadInput(cur, fg_thread, True))
    ok = bool(_u32.SetForegroundWindow(hwnd))
    time.sleep(0.3)
    if attached:
        _u32.AttachThreadInput(cur, fg_thread, False)
    return ok and _u32.GetForegroundWindow() == hwnd


def _press(key: str) -> None:
    """发一次 Ctrl+Z（undo）或 Ctrl+Shift+Z（redo）。

    调用前必须已确认前台是游戏窗口 —— SendInput 只认当前前台。
    """
    seq = [VK_CONTROL]
    if key == "redo":
        seq.append(VK_SHIFT)
    seq.append(VK_Z)
    for vk in seq:
        _send_input(vk, False)
        time.sleep(0.06)
    for vk in reversed(seq):
        _send_input(vk, True)
        time.sleep(0.06)


# ------------------------------------------------------------------ 重编译

_last_key: str | None = None     # 本进程上次发的是哪个键（None = 还没发过）
_MISSING_STACK_HINT = ("电路没有可撤销的编辑历史？先手动加一根线，"
                       "之后就能无限次交替调用")


def _aim(pid: int) -> tuple[int | None, str | None]:
    """找一个可以安全操作的游戏主窗口。

    返回 (hwnd, 错误)。任一项不满足就拒绝 —— 上一次实测的致命教训：
    游戏崩溃后弹出的 CRT 错误框同属游戏进程、也是「可见且有标题」，
    被当成主窗口后又给它发了按键。
    """
    dlg = blocking_dialog(pid)
    if dlg:
        return None, (f"游戏弹出了错误对话框（{dlg[1]}）—— 游戏已处于异常状态，"
                      "拒绝任何操作，请先手动处理它")
    hwnd = find_game_window(pid)
    if not hwnd:
        return None, "找不到游戏主窗口"
    return hwnd, None


def recompile(key: str | None = None) -> Result:
    """触发一次重编译。

    key=None（模块调用的默认）—— 自动交替：上次发了撤销，这次就发重做。
    记下的状态会在进程退出时用来把撤销栈恢复原样（见 restore_stack）。

    key="undo"/"redo"（CLI 用）—— 显式指定，不记状态、退出时不恢复；
    CLI 每次都是新进程，本来也记不住，交替由调用者负责。
    """
    global _last_key
    t = _target()
    if not t:
        return Result(False, "recompile", "游戏未运行")
    pid, _base = t
    hwnd, err = _aim(pid)
    if err or hwnd is None:
        return Result(False, "recompile", err or "找不到游戏主窗口")

    explicit = key is not None
    if key is None:
        key = "redo" if _last_key == "undo" else "undo"
    if key not in ("undo", "redo"):
        return Result(False, "recompile", f"key 只能是 undo/redo，收到 {key!r}")

    if not force_foreground(hwnd):
        fg = _u32.GetForegroundWindow()
        return Result(False, "recompile",
                      f"抢前台失败，已拒绝发键（前台是 {_window_owner(fg)}）")
    # 铁律：SendInput 发给当前前台窗口，前台不是游戏就绝不许发键，
    # 否则会把 Ctrl+Z 打到别人的程序上。
    if _u32.GetForegroundWindow() != hwnd:
        fg_now = _window_owner(_u32.GetForegroundWindow())
        return Result(False, "recompile",
                      f"前台校验未通过，已拒绝发键（前台是 {fg_now}）")
    try:
        _press(key)
    except OSError as e:
        return Result(False, "recompile", str(e))
    if not explicit:
        _last_key = key
    label = "Ctrl+Z 撤销" if key == "undo" else "Ctrl+Shift+Z 重做"
    return Result(True, "recompile", f"已发送 {label}（pid={pid}）")


def restore_stack() -> Result:
    """把撤销栈恢复原状：上次发的若是撤销，就补一次重做（反之亦然）。

    幂等 —— 恢复过就不会再恢复。由 atexit / SIGTERM 自动调用，
    也可手工调用（在长驻程序中途收尾）。
    """
    global _last_key
    if _last_key is None:
        return Result(True, "restore", "无需恢复（本进程没发过键）")
    key = "redo" if _last_key == "undo" else "undo"
    _last_key = None                       # 先清空：保证幂等，失败也不再重试
    t = _target()
    if not t:
        return Result(False, "restore", "游戏已退出，栈状态留在游戏里（本次未恢复）")
    pid, _base = t
    hwnd, err = _aim(pid)
    if err or hwnd is None:
        return Result(False, "restore",
                      (err or "找不到游戏主窗口") + "（栈留在游戏里）")
    if not force_foreground(hwnd) or _u32.GetForegroundWindow() != hwnd:
        return Result(False, "restore", "抢前台失败，未恢复（栈留在游戏里）")
    try:
        _press(key)
    except OSError as e:
        return Result(False, "restore", str(e))
    label = "Ctrl+Z" if key == "undo" else "Ctrl+Shift+Z"
    return Result(True, "restore", f"已补发 {label}，撤销栈恢复原状")


def _on_sigterm(signum, _frame):
    restore_stack()
    raise SystemExit(0)


atexit.register(restore_stack)
if hasattr(signal, "SIGTERM"):
    # 非主线程注册不了 signal，忽略即可
    with contextlib.suppress(ValueError, OSError):
        signal.signal(signal.SIGTERM, _on_sigterm)


# ---------------------------------------------------------------------- CLI

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="仿真控制：run / stop / recompile")
    sub = ap.add_subparsers(dest="action", required=True)
    sub.add_parser("run", help="启动仿真运行")
    sub.add_parser("stop", help="停止仿真")
    p_rec = sub.add_parser("recompile", help="触发一次重编译（注入按键）")
    p_rec.add_argument("--redo", action="store_true",
                       help="改发 Ctrl+Shift+Z 重做（默认发 Ctrl+Z 撤销）")
    args = ap.parse_args(argv)

    if args.action == "run":
        r = run()
    elif args.action == "stop":
        r = stop()
    else:
        r = recompile("redo" if args.redo else "undo")
    print(r)
    return 0 if r.ok else 1


if __name__ == "__main__":
    sys.exit(main())

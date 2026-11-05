#!/usr/bin/env python3
"""仿真控制工具：run / stop / recompile。

设计见 docs/specs/2026-10-05-sim-ctl-design.md。

三个动作：
    run()        注入 F6 —— 游戏的「运行 / 暂停」**切换**键
    stop()       注入 F4 —— 游戏「停止」（复位）
    recompile()  注入按键触发重编译（SendInput 发 Ctrl+Z / Ctrl+Shift+Z）

run/stop 原本是写内存（commands[0] = 0 / 3），2026-10-06 改成注入按键：让游戏走
自己那条 UI 路径，UI 状态与复位副作用都由游戏处理。代价是**必须先抢游戏窗口到
前台**，见「抢前台」一节。

⚠️ F6 是**切换**键，不是「确保在运行」：连发两次等于没发。写内存那版是幂等的
（commands[0] 直接置成 0/3），两版语义不同 —— 调用者若要旧的幂等语义，得自己
记住状态。

recompile 走 UI 注入：让游戏自己走「电路变化 → 重建场景 → 编译」那条路。
不开自动检测（不监听 opt.ini），不判断编译是否成功（编译必经 compile.dll，
调试看转发层日志或主频工具）。
⚠️ **recompile 之后仿真处于停止状态**（重建场景会把运行停掉），要测主频必须再 `run()`
把它点回运行 —— 少了这一步，采样器会一直报「仍在停机」并输出「本次没有跑起来的电路」。
同理，改了 `opt.ini`（例如切 `compiler`）之后也要先 `recompile()` 让新配置生效，再 `run()`
恢复运行，然后才采样。

命令行用法：
    python services/simctl.py run
    python services/simctl.py stop
    python services/simctl.py recompile            # 发撤销
    python services/simctl.py recompile --redo     # 发重做

CLI 每次都是新进程。脚本内循环请 import 本模块，recompile() 会按净消耗交替，
并在进程退出时把撤销栈恢复原状。

**记账**：撤销是消耗性操作（每发一次就少一步历史），所以记的是「净消耗步数」，
退出时按它补发等量反向键 —— 自动交替与显式 key 一视同仁。CLI 是一次性进程，
收尾时会主动放弃补偿（否则一次调用变成「撤销+重做」成对发送，多一次编译）。

CLI 还会在退出前**把焦点还给调用前那个窗口**（只在焦点确实被本工具改过、且此刻
仍停在游戏上时才还；用户中途自己切走了就不打扰）。模块调用不做这件事 —— 模块的
生命周期长，中途用户可能自己切焦点。代价是 CLI 一次调用可能要抢两次前台
（一次去、一次回），比模块调用慢约 0.3~0.5 秒。
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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
from libs import tc_common, winmem  # noqa: E402

EXE = tc_common.GAME_EXE_NAME
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
VK_F4, VK_F6 = 0x73, 0x75        # F4 停止（复位）/ F6 运行-暂停切换
GW_OWNER = 4
SW_RESTORE = 9      # ShowWindow：还原最小化的窗口


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
# ⚠ run/stop 已改走按键注入（见「运行 / 停止（注入按键）」），本节这套写内存
# 通道保留作逆向记录与备用手段：_publish 是「直接按游戏命令协议置位」的完整
# 实现（幂等，不抢前台），在需要无窗口交互的场合仍然有用。

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


# ------------------------------------------------- 运行 / 停止（注入按键）

# 早先这两个动作是**写内存**：settings[1]=INT64_MAX、commands[0]=0(run)/3(quit)、
# commands[1]=新 id、simulation_next_command_id=新 id+1（实现见下面的 _publish，
# 保留作逆向记录）。改走按键后，游戏用自己的 UI 路径处理这两个动作。
# 语义差别要知道：写内存是**幂等置位**，F6 是**切换**（连发两次等于没发）。
# 具体实现放在下面「窗口 / 抢前台 / 按键」一节之后（与 recompile 共用同一套
# 找窗口 + 抢前台 + 校验流程）。


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
_u32.IsWindow.argtypes = [wintypes.HWND]
_u32.IsWindow.restype = wintypes.BOOL
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

    **最小化的窗口先还原**（SW_RESTORE）：窗口最小化时，前台就算已经是他，按键
    也到不了 —— 实测 IsIconic=True 时发 Ctrl+Z，游戏毫无反应；还原后立刻生效。

    只在**真的最小化**时调 ShowWindow —— 窗口正常时调它会让 ImGui 多走一遍
    还原/重绘路径：2026-10-06 那次游戏崩溃（game_engine.dll 的 ImGui 断言
    `scale > 0.0f`）就是窗口本来正常、却多余地调了它。在真最小化的场景下实测
    安全（游戏毫发无损），因为那正是它该凎的活。

    刻意不调 BringWindowToTop —— 抢前台不需要它，而它同样会改窗口显示状态。
    """
    # 最小化 → 先还原。这一步要放在「已经是前台就直接返回」之前：最小化窗口
    # 完全可能“前台已经是它”而按键仍然无效。
    if _u32.IsIconic(hwnd):
        _u32.ShowWindow(hwnd, SW_RESTORE)
        time.sleep(0.4)

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

_undo_balance: int = 0           # 本进程净发出的 undo 次数（每次 undo +1，redo −1）
                                 # >0 = 欠着 undo，退出时按它补发等量 redo
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


def _tap(vk: int) -> None:
    """发一次「按下 → 抬起」（单键，无修饰键）。

    调用前必须已确认前台是游戏窗口 —— SendInput 只认当前前台。
    """
    _send_input(vk, False)
    time.sleep(0.06)
    _send_input(vk, True)
    time.sleep(0.06)


def _send_game_key(action: str, vk: int, label: str) -> Result:
    """共用的发键流程：找窗口 → 抢前台 → 校验前台 → 发键。

    与 recompile 同一条铁律：SendInput 发给**当前前台窗口**，抢不到前台就绝不
    发键，否则按键会落到别人的程序上（实测误发过 VS Code）。
    按键是异步的，发完即返回，**不判断游戏那边做了什么**（沿用设计原则）。
    """
    t = _target()
    if not t:
        return Result(False, action, "游戏未运行")
    pid, _base = t
    hwnd, err = _aim(pid)
    if err or hwnd is None:
        return Result(False, action, err or "找不到游戏主窗口")
    if not force_foreground(hwnd):
        fg = _u32.GetForegroundWindow()
        return Result(False, action,
                      f"抢前台失败，已拒绝发键（前台是 {_window_owner(fg)}）")
    if _u32.GetForegroundWindow() != hwnd:
        fg_now = _window_owner(_u32.GetForegroundWindow())
        return Result(False, action,
                      f"前台校验未通过，已拒绝发键（前台是 {fg_now}）")
    try:
        _tap(vk)
    except OSError as e:
        return Result(False, action, str(e))
    return Result(True, action, f"已发送 {label}（pid={pid}）")


def run() -> Result:
    """运行 / 暂停 —— 注入 F6。

    注意是**切换**：正在跑的时候调用它会暂停（写内存那版是幂等置位）。
    """
    return _send_game_key("run", VK_F6, "F6（运行/暂停切换）")


def stop() -> Result:
    """停止（复位）—— 注入 F4。"""
    return _send_game_key("stop", VK_F4, "F4（停止/复位）")


def recompile(key: str | None = None) -> Result:
    """触发一次重编译。

    key=None（模块调用的默认）—— 按净消耗交替：欠着撤销就发重做，否则发撤销。

    key="undo"/"redo" —— 显式指定。**同样记账**：模块连续发 5 次撤销，退出时
    会补发 5 次重做（见 restore_stack）。CLI 是例外，它退出前主动放弃补偿
    （forget_restore）。
    """
    global _undo_balance
    t = _target()
    if not t:
        return Result(False, "recompile", "游戏未运行")
    pid, _base = t
    hwnd, err = _aim(pid)
    if err or hwnd is None:
        return Result(False, "recompile", err or "找不到游戏主窗口")

    if key is None:
        # 净消耗为正 = 欠着 undo，这次该发 redo 抵掉
        key = "redo" if _undo_balance > 0 else "undo"
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
    _undo_balance += 1 if key == "undo" else -1
    label = "Ctrl+Z 撤销" if key == "undo" else "Ctrl+Shift+Z 重做"
    owed = "" if _undo_balance == 0 else f"，净欠 {_undo_balance} 步"
    return Result(True, "recompile", f"已发送 {label}（pid={pid}{owed}）")


def restore_stack() -> Result:
    """把撤销栈恢复原状：按本次的**净消耗步数**补发等量反向键。

    撤销是消耗性操作，所以记的是「净发了几次 undo」而不是「最后发了哪个键」
    —— 后者在显式 key 连着发 N 次时表达不了 N（只能补 1 次）。

    幂等 —— 恢复过就不会再恢复。由 atexit / SIGTERM 自动调用，
    也可手工调用（在长驻程序中途收尾）。
    """
    global _undo_balance
    n = _undo_balance
    _undo_balance = 0                      # 先清零：保证幂等，失败也不再重试
    if n == 0:
        return Result(True, "restore", "无需恢复（净消耗为 0）")
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
    key = "redo" if n > 0 else "undo"
    label = "Ctrl+Shift+Z 重做" if n > 0 else "Ctrl+Z 撤销"
    try:
        for _ in range(abs(n)):
            _press(key)
    except OSError as e:
        return Result(False, "restore", str(e))
    return Result(True, "restore", f"已补发 {abs(n)} 次 {label}，撤销栈恢复原状")


def forget_restore() -> None:
    """放弃退出时的补偿 —— 本进程的动作由调用者自己负责。

    CLI 用。它是一次性进程，一次调用只做一个动作；不放弃的话 atexit 会补发
    反向键，把动作变成「撤销+重做」成对发送（平白多一次编译 —— 编译近 1 秒，
    调用方明确不要）。
    """
    global _undo_balance
    _undo_balance = 0


def _on_sigterm(signum, _frame):
    restore_stack()
    raise SystemExit(0)


atexit.register(restore_stack)
if hasattr(signal, "SIGTERM"):
    # 非主线程注册不了 signal，忽略即可
    with contextlib.suppress(ValueError, OSError):
        signal.signal(signal.SIGTERM, _on_sigterm)


def _restore_focus(before: int) -> None:
    """CLI 收尾：把焦点还给调用前那个窗口（best-effort，失败静默）。

    三个条件全满足才动手，否则宁可什么都不做：
    1. `before` 是个真实存在的窗口（不是 0 / 桌面）；
    2. 焦点确实变了（没变就无需归还）；
    3. **此刻焦点仍停在游戏窗口上** —— 若这期间用户自己切走了，再抢回来
       等于抢用户的焦点，比不还更糟。

    模块调用不经这里（模块生命周期长，中途用户可能自己切焦点）。
    """
    if not before or before == _u32.GetForegroundWindow():
        return
    if not _u32.IsWindow(before):
        return
    t = _target()
    if not t:
        return
    game = find_game_window(t[0])
    if not game or _u32.GetForegroundWindow() != game:
        return
    force_foreground(before)


# ---------------------------------------------------------------------- CLI

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="仿真控制：run / stop / recompile")
    sub = ap.add_subparsers(dest="action", required=True)
    sub.add_parser("run", help="运行 / 暂停（注入 F6，是切换键）")
    sub.add_parser("stop", help="停止 / 复位（注入 F4）")
    p_rec = sub.add_parser("recompile", help="触发一次重编译（注入按键）")
    p_rec.add_argument("--redo", action="store_true",
                       help="改发 Ctrl+Shift+Z 重做（默认发 Ctrl+Z 撤销）")
    args = ap.parse_args(argv)

    before = _u32.GetForegroundWindow()   # 收尾要把焦点还给它（只对 CLI 这么做）
    if args.action == "run":
        r = run()
    elif args.action == "stop":
        r = stop()
    else:
        r = recompile("redo" if args.redo else "undo")
        forget_restore()   # 一次性进程：动作已由调用者掌控，别让 atexit 再补发
    print(r)
    _restore_focus(before)
    return 0 if r.ok else 1


if __name__ == "__main__":
    sys.exit(main())

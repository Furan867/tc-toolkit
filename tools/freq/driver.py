"""驱动游戏的薄封装 —— `simctl` 之上加**前置断言**（设计 §4「前置断言（失败要吵）」）。

两层：

    run / stop / recompile / restore / blocking_dialog
        薄封装 `simctl`，把它的 `Result` 换成带**可执行失败解释**（`hint`）的形式，
        一律**不抛异常**。`simctl` 的对外接口不变，本模块只在它上面分层。
    preflight()
        跑测量前把六条前提逐条查一遍：**游戏在跑 · PID 稳定 · 撤销栈非空 ·
        能抢前台 · 无阻塞弹窗 · 偏移可读**。每条失败都给下一步该做什么。

⚠️ **撤销栈非空**那条**无法前置探测** —— 游戏不暴露栈深，栈空时 `Ctrl+Z` 静默
无效，「`hook.log` 无新行」是唯一可靠判据（`docs/specs/…freq…§6` 与
`tools/simctl.py` 的说明）。所以它带 `verifiable=False`：查不了，只能在第一次
`recompile` 之后由 `products.wait_new_product` 的失败解释指出来。
"""
from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from .. import simctl, winmem
from . import probe


@dataclass(frozen=True)
class Result:
    """一次动作的结果 —— 沿用 `simctl.Result` 的形状，多一个可执行的 `hint`。"""

    ok: bool
    action: str
    detail: str = ""
    hint: str = ""

    def __bool__(self) -> bool:
        return self.ok

    def __str__(self) -> str:
        head = "ok" if self.ok else "失败"
        tail = f"  {self.detail}" if self.detail else ""
        more = f"  → {self.hint}" if (not self.ok and self.hint) else ""
        return f"{head}: {self.action}{tail}{more}"


@dataclass(frozen=True)
class Check:
    """一条前置断言。`verifiable=False` 表示当前**查不了**（不是通过）。"""

    name: str
    ok: bool
    detail: str = ""
    hint: str = ""
    verifiable: bool = True

    def __str__(self) -> str:
        mark = "ok" if self.ok else "✗"
        if not self.verifiable:
            mark = "?"
        tail = f"  {self.detail}" if self.detail else ""
        more = f"  → {self.hint}" if (not self.ok and self.hint) else ""
        return f"[{mark}] {self.name}{tail}{more}"


# ---------------------------------------------------------------- 动作（薄封装 simctl）

def _adapt(r: simctl.Result, hint: str) -> Result:
    return Result(r.ok, r.action, r.detail, "" if r.ok else hint)


_GAME_GONE = "先在 Steam 里把游戏启起来，再重跑本命令"
_NEED_FOREGROUND = ("抢不到前台就不发键（SendInput 只认前台）—— 别把游戏最小化、"
                    "别切走窗口；跨虚拟桌面抢前台会把你拽回游戏桌面，注意时机")


def run() -> Result:
    """运行 / 暂停 —— 注入 F6（**切换**键，不是幂等的「确保在运行」）。

    要「确保在运行」得先 `stop()`（F4，幂等）把状态摁到停止，再 `run()`。
    """
    return _adapt(simctl.run(), f"{_GAME_GONE}；{_NEED_FOREGROUND}")


def stop() -> Result:
    """停止 / 复位 —— 注入 F4（幂等，多按无副作用）。"""
    return _adapt(simctl.stop(), _GAME_GONE)


def recompile(key: str | None = None) -> Result:
    """触发一次重编译（注入 Ctrl+Z / Ctrl+Shift+Z）；`key=None` 时按净消耗交替。

    重编译靠撤销电路触发，所以**撤销栈必须非空**：栈空时按键静默无效，看不到新产物。
    """
    return _adapt(simctl.recompile(key), (
        "撤销栈空了？在游戏里做几次编辑（加一根线）就能继续；"
        f"或 {_NEED_FOREGROUND}"))


def restore() -> Result:
    """把撤销栈恢复原状（按净消耗补发反向键）—— 收尾用，幂等。"""
    return _adapt(simctl.restore_stack(), "游戏已退出，栈状态留在游戏里（下次连上再补发）")


def blocking_dialog(pid: int) -> tuple[int, str] | None:
    """游戏是否弹出了错误对话框（CRT assert 框等）。有就别碰它 —— 它会吃掉按键。"""
    return simctl.blocking_dialog(pid)


def find_window(pid: int) -> int | None:
    """游戏主窗口句柄（排除对话框与有 owner 的窗口）。"""
    return simctl.find_game_window(pid)


# ---------------------------------------------------------------- 前置断言

def _blocked(names: list[str], detail: str, hint: str,
             verifiable: bool = False) -> list[Check]:
    return [Check(name, False, detail, hint, verifiable) for name in names]


def preflight(expect_pid: int | None = None,
              read_hz: Callable[[int, int], float | None] = probe.read_hz,
              open_process: Callable[[int], int] = winmem.open_process) -> list[Check]:
    """跑测量前的六条前置断言。**不抛异常**，全部以 `Check` 返回。

    `expect_pid` 给「这次会话开始时记下的 PID」：进程变了（游戏重启过）意味着一整套
    内存偏移失效，必须重取。`read_hz` / `open_process` 留作注入点，离线也能调到
    这一段。
    """
    found = probe.find_game()
    if not found:
        return [Check("游戏在跑", False, f"{probe.EXE} 没在运行", _GAME_GONE)] + _blocked(
            ["PID 稳定", "偏移可读", "无阻塞弹窗", "能抢前台", "撤销栈非空"],
            f"{probe.EXE} 没在运行", _GAME_GONE)
    pid, base = found
    checks = [Check("游戏在跑", True, f"PID {pid}   基址 0x{base:X}")]

    if expect_pid is None or pid == expect_pid:
        checks.append(Check("PID 稳定", True, f"PID {pid}（与本次会话一致）"))
    else:
        checks.append(Check("PID 稳定", False,
                            f"进程变了：记的是 {expect_pid}，现在是 {pid}",
                            "游戏重启过 → 内存偏移全部失效，重跑一次本命令重新取地址"))

    h = open_process(pid)
    if not h or read_hz(h, base) is None:
        checks.append(Check("偏移可读", False,
                            "读不到 sim_speed",
                            "Steam 更新多半让符号位移了 —— 按 docs/facts.md §主频读数 重新定位，"
                            "别去改 probe.py 的常量凑"))
    else:
        checks.append(Check("偏移可读", True, "sim_speed 可读"))

    dlg = blocking_dialog(pid)
    if dlg:
        checks.append(Check("无阻塞弹窗", False, f"游戏弹出了错误对话框：{dlg[1]}",
                            "游戏已处于异常状态，先手动关掉那个框再看 —— 这时发键会被它吃掉"))
    else:
        checks.append(Check("无阻塞弹窗", True, "没有可见的错误对话框"))

    checks.append(_foreground_check(pid))

    checks.append(Check("撤销栈非空", True,
                        "无法前置探测（游戏不暴露栈深）—— 发键后靠 hook.log 新行判定",
                        "栈空时拿不到新产物；`products.wait_new_product` 会给出解释",
                        verifiable=False))
    return checks


def _foreground_check(pid: int) -> Check:
    hwnd = find_window(pid)
    if not hwnd:
        return Check("能抢前台", False, "找不到游戏主窗口（被隐藏了？）",
                     "点开游戏窗口；最小化的窗口会自动还原，但没有窗口就发不了键")
    if simctl.force_foreground(hwnd):
        return Check("能抢前台", True, "已能拿到游戏窗口前台")
    return Check("能抢前台", False, "抢前台失败",
                 f"{_NEED_FOREGROUND}；实测 ShowWindow(SW_RESTORE)/BringWindowToTop 会踩"
                 "ImGui 断言搞崩游戏，别加那两下")

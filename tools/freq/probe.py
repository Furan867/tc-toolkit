"""读游戏内存里的仿真主频 —— **偏移常量与读法唯一的一处**。

坐标（逆向确认，2026-10-03；链接地址 = 模块内偏移 + `BASE_VA`）：

    sim_speed__presenterZutilities_u29531  0x14624D618  float32，单位 Hz
    last_cycle_count                       0x14624D608  int64，上次采样的 cycle 数
    last_speed_check                       0x14624D610  double，上次采样的 Unix 时刻

游戏每秒把 `(cycle 增量)/(时间增量)` 写进 `sim_speed` —— 所以后两个字段能独立复算
同一次读数，两边对得上读数才可信（这就是 UI 上显示的那个 MHz）。

⚠️ Steam 更新会让这些地址位移，读不到 `sim_speed` 时先怀疑它（重新定位的办法与
历史值见 `docs/facts.md` §主频读数），**不要**改这里的常量去凑。

复现：`python -m tools.freq.cli_read --watch` 的读数与 `work/freq_1.txt` 逐条对照。
"""
from __future__ import annotations

import struct
import time
from collections.abc import Callable
from dataclasses import dataclass

from libs import tc_common, winmem

EXE = tc_common.GAME_EXE_NAME
BASE_VA = 0x140000000
LINK_CYCLE = 0x14624D608
LINK_TIME = 0x14624D610
LINK_SIM_SPEED = 0x14624D618

OFF_CYCLE = LINK_CYCLE - BASE_VA
OFF_TIME = LINK_TIME - BASE_VA
OFF_SIM_SPEED = LINK_SIM_SPEED - BASE_VA


@dataclass(frozen=True)
class Snapshot:
    """一次读取里的三个字段。

    `game_time` / `cycle` 单独读不到时置 `None` —— 计时读数仍然有效，只是没有
    复算对照（`hz_from_cycle` 会返回 None）。
    """

    hz: float
    game_time: float | None
    cycle: int | None


def read_hz(h: int, base: int) -> float | None:
    """读游戏自己算的仿真主频（float32 Hz）。读不到返回 None。"""
    raw = winmem.rpm(h, base + OFF_SIM_SPEED, 4)
    return None if raw is None else struct.unpack_from("<f", raw)[0]


def read_speed_time(h: int, base: int) -> tuple[float, float] | None:
    """(主频, 游戏自算主频用的那个时刻) —— 采样器按这个时刻做相位对齐。"""
    hz = read_hz(h, base)
    raw_time = winmem.rpm(h, base + OFF_TIME, 8)
    if hz is None or raw_time is None:
        return None
    return hz, struct.unpack_from("<d", raw_time)[0]


def snapshot(h: int, base: int) -> Snapshot | None:
    """一次读全三个字段 —— 复算 Hz 要求 cycle 与时刻取自同一个区间。

    `sim_speed` 读不到就整体返回 None：偏移过时或没权限时，另外两个字段也没有
    意义（那时读数只会误导）。
    """
    hz = read_hz(h, base)
    if hz is None:
        return None
    raw_time = winmem.rpm(h, base + OFF_TIME, 8)
    raw_cycle = winmem.rpm(h, base + OFF_CYCLE, 8)
    return Snapshot(
        hz,
        None if raw_time is None else struct.unpack_from("<d", raw_time)[0],
        None if raw_cycle is None else struct.unpack_from("<q", raw_cycle)[0],
    )


def hz_from_cycle(prev: Snapshot, cur: Snapshot) -> float | None:
    """`(Δcycle)/(Δ时刻)` —— 拿 cycle 独立复算游戏自己那套算法。算不出返回 None。

    停机时 `last_cycle_count` 冻结、而 `last_speed_check` 照常每秒刷新
    （`docs/facts.md` §采样判据 ④），所以判断仿真在不在跑要看 **cycle 有没有前进**，
    不能看时刻有没有变。
    """
    if prev.cycle is None or cur.cycle is None:
        return None
    if prev.game_time is None or cur.game_time is None:
        return None
    dt = cur.game_time - prev.game_time
    if dt <= 0:
        return None
    return (cur.cycle - prev.cycle) / dt


def find_game() -> tuple[int, int] | None:
    """游戏在跑、且模块基址拿得到时返回 (pid, 基址)；否则 None。只读，不碰游戏。"""
    pid = winmem.find_pid(EXE)
    if not pid:
        return None
    base, _path = winmem.find_module(pid, EXE)
    return (pid, base) if base else None


def wait_for_game(timeout: float | None = None,
                  say: Callable[[str], None] = print) -> tuple[int, int] | None:
    """等游戏出现，返回 (pid, 基址)；`timeout` 秒内没等到返回 None。

    文案由调用方通过 `say` 注入（本层不决定长什么样、上不上色）。超时那条必须写清
    「接下来该做什么」—— 等不到游戏通常是压根没启动，或进程名被 Steam 更新改过。
    """
    deadline = None if timeout is None else time.time() + timeout
    hinted = False
    while True:
        found = find_game()
        if found:
            return found
        if deadline is not None and time.time() >= deadline:
            say(f"等了 {timeout:.0f}s 也没等到 {EXE} —— 先在 Steam 里把游戏启起来；"
                f"若进程名不是 {EXE}（Steam 更新改过），EXE 与全部偏移都要重新取")
            return None
        if not hinted:
            say(f"等 {EXE} 启动 …（只读内存，不影响游戏）")
            hinted = True
        time.sleep(2.0)

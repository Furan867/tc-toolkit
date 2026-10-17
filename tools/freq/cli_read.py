#!python3
"""薄 CLI：读游戏当前实测的仿真主频（UI 上显示的那个 MHz）。

    python -m tools.freq.cli_read            # 采一次读数 + 一次独立复算对照
    python -m tools.freq.cli_read --watch    # 持续打印（Ctrl-C 停）
    python -m tools.freq.cli_read --samples 5

偏移与读法在 `tools/freq/probe.py`（唯一一处）。**采样口径不在这里** —— 分段、
相位对齐、停机判据、丢首个那些属于 `sampler`（尚未迁移）；本入口只回答「游戏在
跑吗、现在多少 MHz」这种一次性问题。⚠️ 判据入口请不要解析这里的终端输出，
要数字走 `cli_bench --json`。
"""
from __future__ import annotations

import argparse
import sys
import time

from .. import winmem
from . import probe


def _line(prev: probe.Snapshot, cur: probe.Snapshot) -> str:
    """一行读数：游戏自己算的 Hz + 用 cycle/时刻独立复算同一区间的对照。"""
    if cur.hz == 0:
        return "sim_speed        0        ← 仿真没在跑（哨兵值）"
    text = f"sim_speed  {cur.hz / 1e6:8.3f} MHz   ({int(cur.hz)} Hz)"
    calc = probe.hz_from_cycle(prev, cur)
    if calc is not None:
        text += f"    自算（cycle/时间） {calc / 1e6:8.3f} MHz"
    return text


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="读游戏当前实测主频")
    ap.add_argument("--watch", action="store_true", help="一直打印（Ctrl-C 停）")
    ap.add_argument("--samples", type=int, default=2,
                    help="采几次（默认 2：一次读数 + 一次用于复算的对照）")
    ap.add_argument("--interval", type=float, default=1.0,
                    help="采样间隔秒（默认 1.0，与游戏重算节奏一致；再密不产生新信息）")
    args = ap.parse_args(argv)

    found = probe.find_game()
    if not found:
        print(f"{probe.EXE} 没在运行", file=sys.stderr)
        return 1
    pid, base = found
    h = winmem.open_process(pid)

    prev = probe.snapshot(h, base)
    if prev is None:
        print("读不到 sim_speed —— 进程没权限或符号偏移过时（Steam 更新会让它位移）",
              file=sys.stderr)
        return 1
    print(f"PID {pid}   模块基址 0x{base:X}")

    rounds = None if args.watch else max(1, args.samples - 1)
    try:
        while rounds is None or rounds > 0:
            if rounds is not None:
                rounds -= 1
            time.sleep(args.interval)
            cur = probe.snapshot(h, base)
            if cur is None:            # 读失败（进程退出/权限）：保留上次读数当基线
                print("sim_speed        读不到（偏移过时？）", file=sys.stderr)
                continue
            print(_line(prev, cur), flush=True)
            prev = cur
    except KeyboardInterrupt:
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!python3
"""读游戏当前实测的仿真主频（就是 UI 显示的那个 MHz），不用人眼读。

坐标（逆向确认，2026-10-03）
--------------------------
    sim_speed__presenterZutilities_u29531
        链接地址 0x14624d618 → 模块内偏移 0x624D618，**float32，单位 Hz**
    它每秒更新一次，来源是紧邻的两个变量：
        last_cycle_count  (0x624D608)  上次采样的 cycle 数（int）
        last_speed_check  (0x624D610)  上次采样的 Unix 时间戳（double）
    即 (cycle 增量) / (时间增量)。

    本工具把两条路径的结果一起打出来互相校验 —— 两边对得上，读数才可信。
    两者都是 0 / -1 时表示仿真还没跑起来（哨兵值）。

用法
----
    py tools/read_freq.py                # 采一次给读数，再采一次给自算对照
    py tools/read_freq.py --watch        # 持续打印
    py tools/read_freq.py --samples 5    # 连采 5 次
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import winmem

EXE = "Turing Complete.exe"
BASE_VA = 0x140000000
OFF_CYCLE = 0x14624D608 - BASE_VA
OFF_TIME = 0x14624D610 - BASE_VA
OFF_SIM_SPEED = 0x14624D618 - BASE_VA


def read_field(h, addr, fmt, size):
    raw = winmem.rpm(h, addr, size)
    return None if raw is None else struct.unpack_from(fmt, raw)[0]


def main() -> int:
    ap = argparse.ArgumentParser(description="读游戏当前实测主频")
    ap.add_argument("--watch", action="store_true", help="一直打印（Ctrl-C 停）")
    ap.add_argument("--samples", type=int, default=2, help="采几次（默认 2）")
    ap.add_argument("--interval", type=float, default=1.0, help="采样间隔秒（默认 1.0）")
    args = ap.parse_args()

    pid = winmem.find_pid(EXE)
    if not pid:
        print(f"{EXE} 没在运行", file=sys.stderr)
        return 1
    base, _path = winmem.find_module(pid, EXE)
    if base is None:
        print("找不到模块", file=sys.stderr)
        return 1
    h = winmem.open_process(pid)

    def sample():
        return (read_field(h, base + OFF_SIM_SPEED, "<f", 4),
                read_field(h, base + OFF_CYCLE, "<q", 8),
                read_field(h, base + OFF_TIME, "<d", 8))

    print(f"PID {pid}   模块基址 0x{base:X}")
    prev = sample()
    if prev[0] is None:
        print("读不到 sim_speed —— 进程没权限或符号偏移过时（Steam 更新会让它位移）",
              file=sys.stderr)
        return 1

    rounds = 10 ** 9 if args.watch else max(1, args.samples - 1)
    for _ in range(rounds):
        time.sleep(args.interval)
        cur = sample()
        hz = cur[0]
        if hz is None:
            print("sim_speed        读不到（偏移过时？）", file=sys.stderr)
        elif hz == 0:
            print("sim_speed        0        ← 仿真没在跑（哨兵值）")
        else:
            line = f"sim_speed  {hz / 1e6:8.3f} MHz   ({int(hz)} Hz)"
            if prev[1] is not None and cur[1] is not None and prev[2] and cur[2] > prev[2]:
                dt = cur[2] - prev[2]
                line += f"    自算（cycle/时间） {((cur[1] - prev[1]) / dt) / 1e6:8.3f} MHz"
            print(line, flush=True)
        prev = cur
    return 0


if __name__ == "__main__":
    sys.exit(main())

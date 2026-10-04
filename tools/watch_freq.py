#!python3
"""常驻采样器：被动记录游戏里实测的仿真主频，并标出每次编译点。

为什么是被动的
--------------
游戏每秒重算一次实测主频（`sim_speed`，float32 Hz，模块内偏移 0x624D618）。
本工具按同一节奏轮询它，把样本落进 `work/opt/freq_log.tsv`，同时在终端实时
显示。切配置 / 重编译电路之后**不需要再叫人跑脚本**。

实测判据（2026-10-04，数据见 work/freq_1.txt）
---------------------------------------------
    暂停 / 停止        → 0            （游戏自己把统计归零）
    电路自带速度上限   → 停在上限值   （实测电路 B 限 10 kHz：非 0 但极低）
    启动 / 停止那一秒  → 一个混合值   （窗口横跨切换点，会自成小段）

所以 **低值不能当噪声丢**，只按值分段；样本数 < 3 的段判为"过渡"，不计入统计。
混合值天然自成 1 样本的小段，因此不需要额外丢弃段首尾。

产物 ↔ 电路
----------
编译点从 `work/opt/optimize.log` 取该产物的 **sha256 前 8 位**当"电路指纹"——
同一电路反复编译指纹相同，日志里据此能把重复测量自动归组。

用法
----
    py tools/watch_freq.py                  # Ctrl-C 停
    py tools/watch_freq.py --seconds 30     # 记 30 秒后自动收尾
    py tools/watch_freq.py --interval 0.25  # 轮询更密（游戏仍是每秒才换值）
"""
from __future__ import annotations

import argparse
import os
import re
import statistics
import struct
import sys
import time
import unicodedata
from datetime import datetime
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import term
import winmem

ROOT = Path(__file__).resolve().parent.parent
HOOK_DIR = ROOT / "work" / "hook"
OUT_DIR = ROOT / "work" / "opt"
LOG_PATH = OUT_DIR / "freq_log.tsv"
OPT_INI = ROOT / "opt.ini"
OPT_LOG = OUT_DIR / "optimize.log"

EXE = "Turing Complete.exe"
BASE_VA = 0x140000000
OFF_CYCLE = 0x14624D608 - BASE_VA
OFF_TIME = 0x14624D610 - BASE_VA
OFF_SIM_SPEED = 0x14624D618 - BASE_VA

BAR_UNIT_HZ = 0.7e6      # 柱状图每格代表的频率（满 14 格 ≈ 9.8 MHz）
BAR_WIDTH = 14
IDLE_HEARTBEAT = 30.0    # 停机时每隔多少秒给一行心跳（免得看着像卡死）
SEG_TOL = 0.05           # 段内允许的相对波动
POLL = 0.1               # 轮询间隔（只为及时捕捉“游戏刚重算”的时刻）


def disp_width(text: str) -> int:
    """终端显示宽度 —— 中日韩全角字符占 2 列，len() 会算少，表格就歪了。"""
    return sum(2 if unicodedata.east_asian_width(ch) in "WF" else 1 for ch in text)


def pad(text: str, width: int) -> str:
    """按**显示宽度**补齐（对齐必须在着色之前完成）。"""
    return text + " " * max(0, width - disp_width(text))


def fmt_hz(hz: float) -> str:
    """终端用的频率串，固定 13 显示列宽。"""
    if hz == 0:
        return pad("停机", 13)
    return f"{hz / 1e6:9.3f} MHz"


def bar(hz: float) -> str:
    """固定 BAR_WIDTH 显示列的柱状条（未着色）。

    非零值**至少给一格** —— 电路自带的速度上限可能只有几 kHz，按比例算柱子
    会全空，那就等于没有信息（实测电路 B 限 10 kHz 时就是这情况）。
    """
    if hz == 0:
        return " " * BAR_WIDTH
    n = max(1, min(BAR_WIDTH, int(hz / BAR_UNIT_HZ)))
    return "▇" * n + " " * (BAR_WIDTH - n)


def hz_style(hz: float) -> tuple[str, ...]:
    """频率与柱子的配色：低保真频率（多半是电路自带限速）用黄，正常用绿。"""
    return ("bold", "yellow") if hz < 1e6 else ("bold", "green")


class Sink:
    """终端 + 日志双写：终端给人看，日志给脚本读（TSV，不掺颜色）。"""

    def __init__(self, log_path: Path):
        log_path.parent.mkdir(parents=True, exist_ok=True)
        fresh = not log_path.exists()
        self.fh = log_path.open("a", encoding="utf-8")
        if fresh:
            self.fh.write("# tools/watch_freq.py 追加写（TSV）\n")
            self.fh.write("# 列：时间\t类型\t值(Hz)\tcycle\t备注\n")
            self.fh.flush()

    def write(self, kind: str, hz: float | None = None, cycle: int | None = None,
              note: str = "") -> None:
        self.fh.write(
            f"{datetime.now():%Y-%m-%d %H:%M:%S}\t{kind}\t"
            f"{'' if hz is None else round(hz)}\t"
            f"{'' if cycle is None else cycle}\t{note}\n")
        self.fh.flush()

    def close(self) -> None:
        self.fh.close()


class Segmenter:
    """把连续相近的样本归成段。低值不作噪声处理（可能是电路自带的上限）。"""

    def __init__(self, on_close):
        self.on_close = on_close
        self.samples: list[tuple[float, float]] = []      # (本地时刻, Hz)
        self.closed = 0                                   # 已关闭的段数

    def value(self) -> float:
        return statistics.median(hz for _t, hz in self.samples)

    def add(self, t: float, hz: float) -> bool:
        """喂一个样本；返回 True 表示由此**开了一段新段**。

        ⚠️ 开新段时必须**先收尾旧段、再把本样本收进新段**。早期版本在 close()
        之后直接 return，把新段的第一个样本悄悄丢了（计数从 0 开始、每段少一个
        样本），见 2026-10-04 的终端输出 `段2·0`。
        """
        started = False
        if self.samples:
            ref = self.value()
            if hz == 0 or ref == 0:
                same = hz == 0 and ref == 0
            else:
                same = abs(hz - ref) <= ref * SEG_TOL
            if not same:
                self.close()
                started = True
        self.samples.append((t, hz))
        return started

    def close(self) -> None:
        if not self.samples:
            return
        self.closed += 1
        self.on_close(self.closed, self.samples)
        self.samples = []

    def no(self) -> int:
        """当前段的编号。"""
        return self.closed + 1

    def count(self) -> int:
        return len(self.samples)


def scan_hook_dir(seen: set[str]) -> list[str]:
    """返回新出现的**产物**文件名（首次调用只是登记现状，不算新）。

    只认 `hook_out_*.bin`：一次编译会先后落 `hook_src_NNN.si` 与
    `hook_out_NNN.bin`，两个都报就成了重复的编译点；而且 `.si` 出现的时刻
    `optimize.log` 可能还没写，指纹 / 改写处数都不完整。
    """
    try:
        names = sorted(p.name for p in HOOK_DIR.iterdir()
                       if p.is_file() and p.name.startswith("hook_out_")
                       and p.suffix == ".bin")
    except OSError:
        return []
    new = [n for n in names if n not in seen]
    seen.clear()
    seen.update(names)
    return new


def opt_info(num: str | None) -> tuple[str, str, str]:
    """取该产物的（指纹, 改写处数, rewrite 开关），读不到就退化成占位符。"""
    rewrite = "?"
    try:
        for line in OPT_INI.read_text(encoding="utf-8").splitlines():
            if line.strip().startswith("rewrite"):
                rewrite = line.split("=", 1)[1].strip()
    except OSError:
        pass
    fp, rewritten = "-", ""
    if num:
        try:
            for line in OPT_LOG.read_text(encoding="utf-8").splitlines():
                if f"{num}.raw.bin" in line:
                    m = re.search(r"sha256 (\w{8})", line)
                    if m:
                        fp = m.group(1)
                    m = re.search(r"改写 (\d+) 处", line)
                    if m:
                        rewritten = f"改写 {m.group(1)} 处"
        except OSError:
            pass
    return fp, rewritten, rewrite


def wait_for_game() -> tuple[int, int]:
    """等游戏出现，返回 (pid, 模块基址)。只读内存，不碰游戏。"""
    hinted = False
    while True:
        pid = winmem.find_pid(EXE)
        if pid:
            base, _path = winmem.find_module(pid, EXE)
            if base:
                return pid, base
        if not hinted:
            print(term.paint(f"等 {EXE} 启动 …（只读内存，不影响游戏）", "yellow"), flush=True)
            hinted = True
        time.sleep(2.0)


def main() -> int:
    ap = argparse.ArgumentParser(description="被动采样游戏实测主频")
    ap.add_argument("--interval", type=float, default=1.0,
                    help="两次样本的最小间隔秒（默认 1.0，与游戏重算节奏一致；再密不产生新信息）")
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="记录指定秒数后自动收尾（默认 0 = 一直记到 Ctrl-C）")
    args = ap.parse_args()

    sink = Sink(LOG_PATH)
    pid, base = wait_for_game()
    h = winmem.open_process(pid)
    sink.write("session", note=f"开始记录 PID={pid} 基址=0x{base:X}")
    print(f"{term.paint('● 已连接', 'bold', 'green')}  PID {term.paint(str(pid), 'bold')}  "
          f"基址 {term.paint(f'0x{base:X}', 'blue')}  "
          f"{term.paint('日志 work/opt/freq_log.tsv  ·  Ctrl-C 结束', 'cyan')}\n")

    def on_segment(index: int, samples: list[tuple[float, float]]) -> None:
        vals = [hz for _t, hz in samples]
        med = statistics.median(vals)
        dur = samples[-1][0] - samples[0][0] + 1.0
        head = f"{term.paint(f'[段 {index}]', 'bold', 'magenta')}"
        if med == 0:
            note = f"段{index} n={len(samples)} 时长={dur:.0f}s 停机"
            sink.write("segment", note=note)
            print(f"   {head}  {term.paint(f'停机 ×{len(samples)} 样本 · {dur:.0f}s', 'yellow')}")
            return
        spread = f"波动 {(max(vals) - min(vals)) / med * 100:.1f}%"
        transitional = len(samples) < 3
        note = (f"段{index} n={len(samples)} 时长={dur:.0f}s 中位数={med:.0f}Hz "
                f"{spread}{' 过渡' if transitional else ''}")
        sink.write("segment", hz=med, note=note)
        print(f"   {head}  {term.paint(fmt_hz(med), *hz_style(med))}  "
              f"{term.paint(f'×{len(samples)} 样本 · {dur:.0f}s', 'cyan')}  "
              f"{term.paint(spread, 'cyan')}"
              f"{term.paint('（过渡：窗口横跨切换点）' if transitional else '', 'yellow')}")

    seg = Segmenter(on_segment)

    def read_all():
        raw_speed = winmem.rpm(h, base + OFF_SIM_SPEED, 4)
        raw_cycle = winmem.rpm(h, base + OFF_CYCLE, 8)
        raw_time = winmem.rpm(h, base + OFF_TIME, 8)
        if raw_speed is None or raw_cycle is None or raw_time is None:
            return None
        return (struct.unpack("<f", raw_speed)[0],
                struct.unpack("<q", raw_cycle)[0],
                struct.unpack("<d", raw_time)[0])

    seen: set[str] = set()
    scan_hook_dir(seen)               # 首次只登记现状，不报成"新编译"
    last_game_time: float | None = None
    sampled_at = 0.0
    idle_rounds = 0
    last_idle_note = 0.0
    deadline = time.time() + args.seconds if args.seconds > 0 else None

    try:
        while True:
            if deadline and time.time() >= deadline:
                break
            time.sleep(min(POLL, args.interval))
            clock = datetime.now().strftime("[%H:%M:%S]")

            for name in scan_hook_dir(seen):
                m = re.search(r"(\d+)", name)
                fp, rewritten, rewrite = opt_info(m.group(1) if m else None)
                extra = f"  {term.paint(rewritten, 'green')}" if rewritten else ""
                print(f"{term.paint(clock, 'blue')}  {term.paint('▣ 编译点', 'bold', 'magenta')} "
                      f"{term.paint(name, 'bold')}  {term.paint('指纹 ' + fp, 'blue')}"
                      f"{extra}  {term.paint('rewrite=' + rewrite, 'cyan')}")
                sink.write("compile", note=f"{name} 指纹={fp} {rewritten} rewrite={rewrite}")
                seg.close()           # 编译点是天然的段分界
                last_game_time = None

            if not winmem.find_pid(EXE):
                sink.write("game", note="游戏已退出，等待重新启动")
                print(f"{term.paint(clock, 'blue')}  {term.paint('○ 游戏退出，等待重启 …', 'yellow')}")
                pid, base = wait_for_game()
                h = winmem.open_process(pid)
                seen.clear()
                scan_hook_dir(seen)
                last_game_time = None
                sink.write("game", note=f"重连 PID={pid} 基址=0x{base:X}")
                continue

            cur = read_all()
            if cur is None:
                continue
            hz, cycle, game_time = cur

            # 停机时游戏的统计会**整体冻结**（连 last_speed_check 都不再更新），所以
            # 不能只靠“游戏换了值”出样本 —— 那样停机期间一行记录也不会有，段长
            # 无从得知。两条腿：游戏一重算就立刻记（相位对齐），否则到点兜底记一次。
            fresh = game_time != last_game_time
            due = time.time() - sampled_at >= args.interval
            if not fresh and not due:
                continue
            if not fresh and idle_rounds == 0:
                sink.write("note", note="游戏统计停更（可能已暂停 / 未运行电路），转为固定节奏记录")
                print(f"{term.paint(clock, 'blue')}  "
                      f"{term.paint('… 游戏统计停更（暂停 / 未运行电路），转为固定节奏记录', 'yellow')}")
                last_idle_note = time.time()
            idle_rounds = 0 if fresh else idle_rounds + 1
            last_game_time = game_time
            sampled_at = time.time()

            started = seg.add(time.time(), hz)
            sink.write("sample", hz, cycle)
            if hz == 0:
                # 停机不逐行刷屏：整段只在结束时汇总一行，中间给低频心跳，
                # 免得看着像卡死。
                if time.time() - last_idle_note >= IDLE_HEARTBEAT:
                    last_idle_note = time.time()
                    print(f"{term.paint(clock, 'blue')}  "
                          f"{term.paint(f'· 停机中（本段已 {seg.count()}s）', 'yellow')}")
                continue
            mark = term.paint("▲", "green") if started else " "
            style = hz_style(hz)
            print(f"{term.paint(clock, 'blue')} {mark} {term.paint(fmt_hz(hz), *style)} "
                  f"{term.paint(bar(hz), *style)} "
                  f"{term.paint(f'段{seg.no()}·{seg.count()}', 'cyan')}")
    except KeyboardInterrupt:
        print()
    finally:
        seg.close()
        sink.write("session", note="结束记录")
        sink.close()
        print(term.paint("■ 已停止，样本已写入 work/opt/freq_log.tsv", "bold", "green"))
    return 0


if __name__ == "__main__":
    sys.exit(main())

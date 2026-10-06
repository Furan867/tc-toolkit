#!python3
"""常驻采样器：被动记录游戏里实测的仿真主频，并标出每次编译点。

为什么是被动的
--------------
游戏每秒重算一次实测主频（`sim_speed`，float32 Hz，模块内偏移 0x624D618）。
本工具按同一节奏轮询它，在终端实时显示，退出时给一份按「编译区间」归并的
总结。切配置 / 重编译电路之后**不需要再叫人跑脚本**。

实测判据（2026-10-04，原始数据 work/freq_1.txt）
-----------------------------------------------
    暂停 / 停止        → 0            （游戏自己把统计归零）
    电路自带速度上限   → 停在上限值   （实测电路 B 限 10 kHz：非 0 但极低）
    启动 / 停止那一秒  → 一个混合值   （窗口横跨切换点，会自成小段）

所以 **低值不能当噪声丢**，只按值分段；样本数 < 3 的段判为"过渡"，不参与统计。

产物 ↔ 电路
----------
编译点从 `work/opt/optimize.log` 取该产物的 **sha256 前 8 位**当"电路指纹"——
同一电路反复编译指纹相同。

读哪里
------
**只看退出时那份总结**（终端上就是，另外还落一份无色的 `work/freq/freq_summary.txt`）。
逐事件的 `work/freq/freq_log.tsv` 只给脚本用，别手动读它 —— 监听几分钟就能摞
几百行，读它只会污染上下文。

用法
----
    py tools/watch_freq.py                  # Ctrl-C 停
    py tools/watch_freq.py --seconds 60     # 记 60 秒后自动收尾
    py tools/watch_freq.py -v               # 总结里附上逐段明细
"""
from __future__ import annotations

import argparse
import hashlib
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
HOOK_DIR = ROOT / "work" / "hook"      # 转发层捕获（每次游戏会话被清空）
OPT_DIR = ROOT / "work" / "opt"        # 转发层工作区（每次游戏会话被清空）
FREQ_DIR = ROOT / "work" / "freq"      # 本工具的输出（跨会话保留，不会被人清掉）
LOG_PATH = FREQ_DIR / "freq_log.tsv"
SUMMARY_PATH = FREQ_DIR / "freq_summary.txt"     # 给人读的总结（每次运行覆盖）
OPT_LOG = OPT_DIR / "optimize.log"          # 改写子进程写的（实际执行值）
# 转发层 hook.log 里每编译一条 `rewrite=<值>`：它是转发层**起改写子进程之前**按
# 当时的 opt.ini 写下的，且 2026-10-05 起会把这个值作为 `--mode` 传给改写器 ——
# 两边同源，正常情况必然一致。万一不一致（历史上 C 侧自己维护过一份白名单，
# 把 short_cse 当非法值归成 on），编译点行会直接把差异标出来，那正是要查的信号。
HOOK_LOG = OPT_DIR / "hook.log"

OPT_WAIT = 15.0        # 见过 hook_out 后等 opt.bin 的上限秒数（转发层自己等子进程 8 秒）

EXE = "Turing Complete.exe"
BASE_VA = 0x140000000
# last_cycle_count 在 0x624D608，本工具不读它 —— "电路是否在推进"靠
# last_speed_check 变不变来判断。
OFF_TIME = 0x14624D610 - BASE_VA
OFF_SIM_SPEED = 0x14624D618 - BASE_VA

BAR_UNIT_HZ = 0.7e6      # 柱状图每格代表的频率（满 14 格 ≈ 9.8 MHz）
BAR_WIDTH = 14
SEG_TOL = 0.05           # 段内允许的相对波动
POLL = 0.1               # 轮询间隔（只为及时捕捉"游戏刚重算"的时刻）


def disp_width(text: str) -> int:
    """终端显示宽度 —— 中日韩全角字符占 2 列，len() 会算少，表格就歪了。"""
    return sum(2 if unicodedata.east_asian_width(ch) in "WF" else 1 for ch in text)


def pad(text: str, width: int) -> str:
    """按**显示宽度**左对齐补齐（对齐必须在着色之前完成）。"""
    return text + " " * max(0, width - disp_width(text))


def pad_left(text: str, width: int) -> str:
    """按**显示宽度**右对齐补齐 —— str.rjust 按字符数算，中文（占 2 列）会多补。"""
    return " " * max(0, width - disp_width(text)) + text


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
    """终端 + 日志双写：终端给人看，日志给脚本读（TSV，不掺颜色）。

    日志**只记事件与段摘要**，不逐样本落盘 —— 后者会让文件长到几百行。
    """

    def __init__(self, log_path: Path):
        log_path.parent.mkdir(parents=True, exist_ok=True)
        fresh = not log_path.exists()
        self.fh = log_path.open("a", encoding="utf-8")
        if fresh:
            self.fh.write("# tools/watch_freq.py 追加写（TSV）\n")
            self.fh.write("# 列：时间\t类型\t值(Hz)\t备注\n")
            self.fh.flush()

    def write(self, kind: str, hz: float | None = None, note: str = "") -> None:
        self.fh.write(f"{datetime.now():%Y-%m-%d %H:%M:%S}\t{kind}\t"
                      f"{'' if hz is None else round(hz)}\t{note}\n")
        self.fh.flush()

    def close(self) -> None:
        self.fh.close()


class Segmenter:
    """把连续相近的样本归成段。低值不作噪声处理（可能是电路自带的上限）。"""

    def __init__(self, on_close):
        self.on_close = on_close
        self.samples: list[tuple[float, float]] = []      # (本地时刻, Hz)

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
        self.on_close(self.samples)
        self.samples = []

    def count(self) -> int:
        return len(self.samples)


def scan_hook_dir(seen: set[str]) -> list[str]:
    """返回新出现的 `hook_out_NNN.bin` 文件名（首次调用只登记现状，不算新）。

    它只用来发现「**编译发生了**」—— 报编译点用的是 `scan_opt_products`（改写后
    的那种产物）。只认 hook_out_*.bin：一次编译会先后落 hook_src_NNN.si 与
    hook_out_NNN.bin，两个都报就成了重复的编译点。
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


def latest_product() -> str | None:
    """当前 opt 目录里最新的**改写后产物**编号（启动时补报"现在跑的是哪个"）。"""
    try:
        names = sorted(p.name[:3] for p in OPT_DIR.iterdir()
                       if p.is_file() and p.name.endswith(".opt.bin")
                       and p.name[:3].isdigit())
    except OSError:
        return None
    return names[-1] if names else None


def scan_opt_products(seen: set[str]) -> list[str]:
    """返回新出现的**改写后产物** `NNN.opt.bin` 的编号（首次只登记现状）。

    编译点以它为准：改写器是「先写产物 → 自校验 → 写 entry → **最后**写日志」，
    所以产物一出现就代表这一趟改写走完了 —— 此时算指纹、读 optimize.log 都有值。
    原来的信号是 `hook_out_NNN.bin`（转发层刚落盘、改写子进程还没跑完），只能
    打占位符。
    """
    try:
        names = sorted(p.name[:3] for p in OPT_DIR.iterdir()
                       if p.is_file() and p.name.endswith(".opt.bin")
                       and p.name[:3].isdigit())
    except OSError:
        return []
    new = [n for n in names if n not in seen]
    seen.clear()
    seen.update(names)
    return new


def product_fp(num: str | None) -> str:
    """指纹：该次编译**实际写回游戏的机器码**（`NNN.opt.bin`）的 sha256 前 8 位。

    自己算，不去读 optimize.log 里那串 sha256 —— 两者同源（日志那行就是改写器
    对这份数据算的），但改写器**最后**才写日志，产物出现的瞬间日志可能还差几毫秒。
    208881 字节实测 28 ms。产物被下一次游戏启动清掉时返回 "-"。
    """
    if not num:
        return "-"
    try:
        h = hashlib.sha256()
        with open(OPT_DIR / f"{num}.opt.bin", "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()[:8]
    except OSError:
        return "-"


def opt_info(num: str | None) -> tuple[str, str, str]:
    """取该产物的（指纹, 改写处数, rewrite 模式），读不到就退化成占位符。

    模式从 optimize.log 的 note 行提取 —— 那是编译时刻的权威记录。
    早先读 opt.ini 的当前值，但那是「检测时刻」的配置：用户在编译间隙
    切开关时必然错位（实测 #005 编译时是 on、标注成了 off，而 005 与 006
    的产物 sha 完全相同，铁证是同模式）。判定产物模式**只认 optimize.log**。
    """
    fp, rewritten, rewrite = "-", "", "?"
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
                    if "CSE 删" in line:
                        rewrite = "short_cse"      # 缩短 + 删重复装载
                    elif "缩短改写" in line:
                        rewrite = "short"          # 纯缩短（不带 CSE）
                    elif "（等长）" in line:
                        rewrite = "on"
                    elif "未改写" in line:
                        rewrite = "off"
        except OSError:
            pass
    return fp, rewritten, rewrite


def mode_from_hook_log(num: str | None) -> str:
    if not num:
        return "?"
    try:
        for line in reversed(HOOK_LOG.read_text(encoding="utf-8").splitlines()):
            m = re.search(rf"\b{num}\s+rewrite=(\S+)", line)
            if m:
                return m.group(1)
    except OSError:
        pass
    return "?"


def summarize(regions: list[dict], verbose: bool = False) -> tuple[str, list[str], str]:
    """按「一次编译 → 一次运行」的区间归并，**一个电路一行**。

    两次编译之间必然是同一次运行，所以每个区间只取**最大的非停机段中位数**当
    代表。不设样本数下限：过渡值（窗口横跨切换点）总是偏低，取最大就天然把它
    排除了；反之，短时间运行（稳定段只有 1~2 个样本）也能被如实报出来 ——
    行内的样本数就是读者判断可靠性的依据。一次都没跑起来的区间省略。
    返回 (标题, **无色**文本行) —— 终端与 freq_summary.txt 共用同一份文字。
    """
    lines: list[str] = []
    digest: list[str] = []
    nth = 0
    total = 0
    for region in regions:
        segments = region["segments"]
        total += sum(len(vals) for _med, vals in segments)
        runs = [(med, vals) for med, vals in segments if med > 0]
        if runs:
            nth += 1
            med, vals = max(runs, key=lambda mv: mv[0])
            # 区间只记编号，摘要时重查 —— 读数只依赖产物与日志，跟当时终端
            # 打印了什么无关。
            fp = product_fp(region["num"])
            _f, rewritten, _rw = (opt_info(region["num"])
                                  if region["num"] else ("-", "", "?"))
            if fp == "-" and _f != "-":
                fp = _f          # 产物已被清（游戏重启过）：退回日志里记的 sha
            rewrite = _rw if _rw != "?" else mode_from_hook_log(region["num"])
            m = re.search(r"(\d+)", rewritten)
            n_rw = m.group(1) if m else "-"
            row = (f"{nth:>2d}) {region['label']:<5s}{med / 1e6:6.3f}"
                   f" {len(vals):>3d}  {(max(vals) - min(vals)) / med * 100:5.2f}"
                   f" {fp:<8s} {n_rw:>5s}  {rewrite}")
            lines.append(row)
            digest.append(f"{region['label']}={med / 1e6:.3f}MHz")
        elif verbose:
            lines.append(f"   --) {region['label']:<8s} 未运行")
        if verbose:
            for med, vals in segments:
                what = "停机" if med == 0 else f"{med / 1e6:8.3f} MHz"
                lines.append(f"      · {region['label']:<8s} {what}"
                             f"  ×{len(vals):<3d} {'过渡' if len(vals) < 3 else ''}")
    title = f"{nth} 个编译区间 · 共 {total} 个样本 · 约 {total}s"
    if lines:
        lines.insert(0, "   #      主频 样本   波动 指纹      改写  rewrite")
    return title, lines, " ".join(digest) or "无运行区间"


def print_summary(regions: list[dict], sink: Sink, verbose: bool = False) -> None:
    """终端打印总结，并把同一份无色文字**覆盖写**进 freq_summary.txt —— 该文件只
    保留最近一次（几十行），取数时读它就行。

    想回溯历史不必另存归档：freq_log.tsv 的 `summary` 行每次都带一行紧凑摘要
    （`#007=6.974MHz #008=6.961MHz …`），`grep summary` 一下就是全部历史，
    不用去读那几百行的事件流。
    """
    title, lines, digest = summarize(regions, verbose)
    print()
    print(term.paint(f"── 总结 ──  {title}", "bold", "magenta"))
    for line in lines:
        print(line)
    if not lines:
        print(term.paint("   （本次没有跑起来的电路）", "yellow"))
    body = lines or ["   （本次没有跑起来的电路）"]
    with SUMMARY_PATH.open("w", encoding="utf-8") as fh:
        fh.write(f"=== {datetime.now():%Y-%m-%d %H:%M:%S}  {title} ===\n")
        fh.write("".join(f"{ln}\n" for ln in body))
    sink.write("summary", note=f"{title} | {digest}")


def read_hz(h: int, base: int) -> float | None:
    """读游戏自己算的仿真主频（float32 Hz，偏移 OFF_SIM_SPEED）。读不到返回 None。"""
    raw = winmem.rpm(h, base + OFF_SIM_SPEED, 4)
    return None if raw is None else struct.unpack_from("<f", raw)[0]


def read_speed_time(h: int, base: int) -> tuple[float, float] | None:
    """(主频, 游戏自算主频用的那个时间戳) —— 后者拿来独立核对 cycle/时间。"""
    hz = read_hz(h, base)
    raw_time = winmem.rpm(h, base + OFF_TIME, 8)
    if hz is None or raw_time is None:
        return None
    return hz, struct.unpack_from("<d", raw_time)[0]


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
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="总结里附上每个区间的逐段明细")
    args = ap.parse_args()

    sink = Sink(LOG_PATH)
    pid, base = wait_for_game()
    h = winmem.open_process(pid)
    sink.write("session", note=f"开始记录 PID={pid} 基址=0x{base:X}")
    print(f"{term.paint('● 已连接', 'bold', 'green')}  PID {term.paint(str(pid), 'bold')}  "
          f"基址 {term.paint(f'0x{base:X}', 'blue')}  "
          f"{term.paint('总结会写进 work/freq/freq_summary.txt  ·  Ctrl-C 结束', 'cyan')}\n")

    regions: list[dict] = []
    on_heartbeat = False

    def say(text: str) -> None:
        """输出一行；若上一条是原地刷新的心跳行，先把位置让出来。"""
        nonlocal on_heartbeat
        if on_heartbeat:
            sys.stdout.write("\n")
            on_heartbeat = False
        print(text, flush=True)

    def new_region(label: str, num: str | None = None) -> None:
        """开一个新区间，只记编号 —— 指纹/模式等摘要时再查。"""
        regions.append({"label": label, "num": num, "segments": []})

    def report_compile(num: str | None, starter: bool = False) -> None:
        """打一条编译点（启动时补报的那条标为"起始"）。

        信号取自**改写后产物** NNN.opt.bin 出现，所以该有的都有：
        - 指纹：自己算这份机器码的 sha256 前 8 位（= 实际写回游戏的那份）；
        - 改写处数 / rewrite：读 optimize.log 的实际执行值。
        若转发层 hook.log 记的配置值与实际执行值不同，直接把差异标出来。"""
        tag = "起始编译" if starter else "编译点"
        when = datetime.now().strftime("[%H:%M:%S]")
        who = f"#{num}" if num else "?"
        fp = product_fp(num)
        _logfp, rewritten, actual = opt_info(num)
        cfg = mode_from_hook_log(num)
        if actual == "?":
            rewrite = cfg if cfg != "?" else "?"
        elif cfg != "?" and cfg != actual:
            rewrite = f"{actual}（hook.log 记 {cfg}）"
        else:
            rewrite = actual
        say(f"{term.paint(when, 'blue')}  {term.paint('▣ ' + tag, 'bold', 'magenta')} "
            f"{term.paint(who, 'bold')}  {term.paint('指纹 ' + fp, 'blue')}"
            + (f"  {term.paint(rewritten, 'green')}" if rewritten else "")
            + f"  {term.paint('rewrite=' + rewrite, 'cyan')}")
        sink.write("compile", note=f"{tag} {who} 指纹={fp} {rewritten} rewrite={rewrite}")

    def on_segment(samples: list[tuple[float, float]]) -> None:
        """段收尾时不打印，只记账：落一行日志 + 存进当前区间（退出时按区间归并）。"""
        vals = [hz for _t, hz in samples]
        med = statistics.median(vals)
        dur = samples[-1][0] - samples[0][0] + 1.0
        region = regions[-1]
        region["segments"].append((med, vals))
        spread = 0.0 if med == 0 else (max(vals) - min(vals)) / med * 100
        sink.write("segment", hz=None if med == 0 else med,
                   note=f"{region['label']} n={len(samples)} 时长={dur:.0f}s "
                        + ("停机" if med == 0 else f"中位数={med:.0f}Hz 波动={spread:.1f}%")
                        + (" 过渡" if len(samples) < 3 else ""))

    seg = Segmenter(on_segment)

    def read_all():
        return read_speed_time(h, base)

    seen_hook: set[str] = set()       # hook_out_*.bin —— 只用来发现“编译发生了”
    seen_opt: set[str] = set()        # NNN.opt.bin   —— 改写走完，用它报编译点
    pending: dict[str, float] = {}    # 见过 hook_out、还没等到 opt 的编号
    scan_hook_dir(seen_hook)
    scan_opt_products(seen_opt)       # 首次只登记现状，不重复报成"新编译"
    starter = latest_product()
    if starter:
        report_compile(starter, starter=True)
        new_region(f"#{starter}", starter)
    else:
        new_region("起始")

    last_game_time: float | None = None
    sampled_at = 0.0
    deadline = time.time() + args.seconds if args.seconds > 0 else None

    try:
        while True:
            if deadline and time.time() >= deadline:
                break
            time.sleep(min(POLL, args.interval))
            clock = datetime.now().strftime("[%H:%M:%S]")

            # 两条腿：hook_out 出现 = 编译发生了；opt.bin 出现 = 改写走完了。
            # 编译点以 opt 为准（那时指纹与日志都就绪），hook_out 只用来记待办，
            # 免得改写器压根没产出时这里一声不响。
            for name in scan_hook_dir(seen_hook):
                m = re.search(r"(\d+)", name)
                if m:
                    pending.setdefault(m.group(1), time.time())
            for num in scan_opt_products(seen_opt):
                pending.pop(num, None)
                seg.close()           # 先让旧段落进旧区间
                report_compile(num)
                new_region(f"#{num}", num)
                last_game_time = None
            for num, t0 in list(pending.items()):
                if time.time() - t0 > OPT_WAIT:
                    pending.pop(num)
                    seg.close()
                    say(f"{term.paint(clock, 'blue')}  "
                        f"{term.paint(f'❌ 编译点 #{num}：改写后产物一直没出现', 'red')}"
                        f"  {term.paint('（看 work/opt/opt_stderr.log）', 'yellow')}")
                    sink.write("compile",
                               note=f"#{num} 改写后产物缺失（超时 {OPT_WAIT:.0f}s）")
                    new_region(f"#{num}", num)
                    last_game_time = None

            if not winmem.find_pid(EXE):
                sink.write("game", note="游戏已退出，等待重新启动")
                say(f"{term.paint(clock, 'blue')}  "
                    f"{term.paint('○ 游戏退出，等待重启 …', 'yellow')}")
                pid, base = wait_for_game()
                h = winmem.open_process(pid)
                seen_hook.clear()
                seen_opt.clear()
                pending.clear()
                scan_hook_dir(seen_hook)
                scan_opt_products(seen_opt)
                last_game_time = None
                sink.write("game", note=f"重连 PID={pid} 基址=0x{base:X}")
                continue

            cur = read_all()
            if cur is None:
                continue
            hz, game_time = cur

            # 停机时游戏的统计会**整体冻结**（连 last_speed_check 都不再更新），
            # 所以不能只靠"游戏换了值"出样本 —— 那样停机期间一行记录也不会有。
            # 两条腿：游戏一重算就立刻记（相位对齐），否则到点兜底记一次。
            fresh = game_time != last_game_time
            due = time.time() - sampled_at >= args.interval
            if not fresh and not due:
                continue
            last_game_time = game_time
            sampled_at = time.time()

            started = seg.add(time.time(), hz)
            if hz == 0:
                # 停机心跳与采样同频，但**原地刷新同一行**，所以不会刷屏。
                sys.stdout.write("\r" + term.paint(
                    f"{clock}  · 仍在停机（已 {seg.count()}s）", "yellow"))
                sys.stdout.flush()
                on_heartbeat = True
                continue
            mark = term.paint("▲", "green") if started else " "
            style = hz_style(hz)
            say(f"{term.paint(clock, 'blue')} {mark} {term.paint(fmt_hz(hz), *style)}"
                f" {term.paint(bar(hz), *style)}")
    except KeyboardInterrupt:
        print()
    finally:
        if on_heartbeat:
            sys.stdout.write("\n")
        seg.close()
        print_summary(regions, sink, args.verbose)
        sink.write("session", note="结束记录")
        sink.close()
        print(term.paint("■ 已停止", "bold", "green"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
